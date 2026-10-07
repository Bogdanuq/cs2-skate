// cs2skate.dll (Astral, loaded by lua/cs2skate.lua): skate in CS2. Starts the Skate 3 Rust Engine hidden, on the
// park made from this map's collision (maps/<map>_link.skate: the engine skates on the same triangles CS2 has) and with
// SKATE_CS2_LINK=<w>x<h>: every engine frame its camera pose, the skater's feet and the skater alone (transparent
// background) come through shared memory Local\Skate3CS2Link (engine: crates/skate-game/src/cs2_link.rs). The Lua puts
// CS2's view on that camera (cs_frame) and this draws the skater over CS2's frame at Present. Your controller drives
// the engine directly (XInput works in the background; the engine is started by Explorer, out of Steam's reach).
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kMagic = 0x4C433353;  // "S3CL"
constexpr size_t kHeader = 4096, kSlotHead = 64, kSlots = 3;
constexpr double kMetresPerUnit = 0.0254;
constexpr float kPi = 3.14159265f;

struct Header {
    uint32_t magic, version, width, height;
    std::atomic<uint32_t> latest, frames;
    std::atomic<uint32_t> players, status_seq;  // online: skaters in the lobby; bumped after each status text
};
constexpr size_t kStatus = 64, kStatusBytes = 256;  // the online status text, in the header
struct SlotHead {
    std::atomic<uint32_t> seq;
    uint32_t pad;
    uint64_t frame;
    float position[3], rotation[4], fov, aspect, root[3];
};
static_assert(sizeof(SlotHead) == kSlotHead);

template <class T>
void Release(T*& p) {
    if (p) p->Release(), p = nullptr;
}

// ---------------------------------------------------------------- messages for the Lua console
std::mutex g_msgLock;
std::deque<std::string> g_messages;
void Say(const char* fmt, ...) {
    char text[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(text, sizeof(text), fmt, a);
    va_end(a);
    std::lock_guard lock(g_msgLock);
    if (g_messages.size() < 64) g_messages.emplace_back(text);
}

// ---------------------------------------------------------------- the engine process and the link
bool g_started = false;
HANDLE g_mapping = nullptr;
uint8_t* g_base = nullptr;
size_t g_mapBytes = 0;

std::wstring Wide(const std::string& s) {
    std::wstring w(MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), int(w.size()));
    return w.resize(w.size() - 1), w;
}

// every running copy of our engine exe (it re-launches itself as a child), matched by full path so the user's own
// skate3rust.exe from the release folder is never touched
std::vector<DWORD> EnginePids(const std::string& exe, bool relay = false) {
    std::vector<DWORD> out;
    const std::wstring want = Wide(exe);
    const std::wstring relayExe = Wide(exe.substr(0, exe.find_last_of("\\/")) + "\\steam-relay\\skate-steam-relay.exe");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        const bool isRelay = relay && !_wcsicmp(e.szExeFile, L"skate-steam-relay.exe");
        if (_wcsicmp(e.szExeFile, L"skate3rust.exe") && !isRelay) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        if (!p) continue;
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n) && !_wcsicmp(path, (isRelay ? relayExe : want).c_str()))
            out.push_back(e.th32ProcessID);
        CloseHandle(p);
    }
    CloseHandle(snap);
    return out;
}

void StopEngine(const std::string& exe) {
    if (g_base) UnmapViewOfFile(g_base), g_base = nullptr;
    if (g_mapping) CloseHandle(g_mapping), g_mapping = nullptr;
    for (DWORD pid : EnginePids(exe, true))  // its Steam relay too: leaves the lobby now, not in 15 s
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) TerminateProcess(p, 0), CloseHandle(p);
    g_started = false;
}

// a process list walk is ~1 ms: look twice a second
bool EngineRunning(const std::string& exe) {
    static ULONGLONG at = 0;
    static bool running = false;
    if (GetTickCount64() - at > 500) at = GetTickCount64(), running = !EnginePids(exe).empty();
    return running;
}

// ShellExecute run by Explorer (its desktop's IShellDispatch2): a child of CS2 gets Steam's overlay injected, and
// that takes the controller away (Steam Input) from the engine
bool ExplorerExecute(const std::wstring& file, const std::wstring& args, const std::wstring& dir) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IShellWindows* windows = nullptr;
    IDispatch *desktop = nullptr, *background = nullptr, *app = nullptr;
    IServiceProvider* services = nullptr;
    IShellBrowser* browser = nullptr;
    IShellView* view = nullptr;
    IShellFolderViewDual* folder = nullptr;
    IShellDispatch2* shell = nullptr;
    VARIANT loc{}, empty{};
    loc.vt = VT_I4, loc.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))) &&
        windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop) == S_OK &&
        SUCCEEDED(desktop->QueryInterface(IID_PPV_ARGS(&services))) &&
        SUCCEEDED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) &&
        SUCCEEDED(browser->QueryActiveShellView(&view)) &&
        SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) &&
        SUCCEEDED(background->QueryInterface(IID_PPV_ARGS(&folder))) && SUCCEEDED(folder->get_Application(&app)) &&
        SUCCEEDED(app->QueryInterface(IID_PPV_ARGS(&shell)))) {
        BSTR f = SysAllocString(file.c_str()), a = SysAllocString(args.c_str()), d = SysAllocString(dir.c_str());
        VARIANT va{}, vd{}, vop{}, vshow{};
        va.vt = VT_BSTR, va.bstrVal = a;
        vd.vt = VT_BSTR, vd.bstrVal = d;
        vshow.vt = VT_I4, vshow.lVal = SW_HIDE;
        ok = SUCCEEDED(shell->ShellExecute(f, va, vd, vop, vshow));
        SysFreeString(f), SysFreeString(a), SysFreeString(d);
    }
    for (IUnknown* p : std::initializer_list<IUnknown*>{shell, app, folder, background, view, browser, services,
                                                        desktop, windows})
        if (p) p->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

// online: 0 off, 1 host, 2 join; name: the skater's name for the lobby (CS2's player name)
bool StartEngine(const std::string& exe, const std::string& assets, const std::string& map, uint32_t w, uint32_t h,
                 int online, const char* name) {
    StopEngine(exe);
    char size[32];
    std::snprintf(size, sizeof(size), "%ux%u", w, h);
    // SKATE_REPORT_CHILD: no crash-report supervisor (its window would pop up over CS2); cs_frame restarts a dead engine
    std::string sets =
        std::string("set SKATE_CS2_LINK=") + size + "&& set SKATE3_INPUT=xinput&& set SKATE_REPORT_CHILD=1&& ";
    if (online == 1 || online == 2) {
        // cmd-safe: letters, digits, space and - _ . only (no & | ^ % < > " in a set)
        std::string safe;
        for (const char* c = name; *c && safe.size() < 16; ++c)
            if (std::isalnum(static_cast<unsigned char>(*c)) || std::strchr(" -_.", *c)) safe += *c;
        while (!safe.empty() && safe.back() == ' ') safe.pop_back();
        sets += std::string("set SKATE_CS2_ONLINE=") + (online == 1 ? "host" : "join") + "&& ";
        if (!safe.empty()) sets += "set SKATE_CS2_NAME=" + safe + "&& ";
    }
    // Explorer's cmd sets the link's switches, then starts the engine (/s: cmd drops only the outer quotes)
    const std::string dir = exe.substr(0, exe.find_last_of("\\/"));
    // the engine's log next to it (engine.log), for crash reports
    const std::string cmd = "/s /c \"" + sets + "\"" + exe + "\" --assets \"" + assets + "\" --map \"" + map +
                            "\" > \"" + dir + "\\engine.log\" 2>&1\"";
    if (!ExplorerExecute(L"cmd.exe", Wide(cmd), Wide(dir)))
        return Say("could not start the skate engine through Explorer (%s)", exe.c_str()), false;
    g_started = true;
    Say("skate engine started (%s, %s%s)", size, map.c_str(), online == 1 ? ", hosting online" : online == 2 ? ", joining online" : "");
    return true;
}

// the engine makes the mapping once it's up: open it then
const Header* Link() {
    if (g_base) return reinterpret_cast<const Header*>(g_base);
    HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\Skate3CS2Link");
    if (!h) return nullptr;
    auto* head = static_cast<uint8_t*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, kHeader));
    if (!head) return CloseHandle(h), nullptr;
    const auto* hd = reinterpret_cast<const Header*>(head);
    const bool good = hd->magic == kMagic && hd->width && hd->height;
    const size_t bytes = kHeader + kSlots * (kSlotHead + size_t(hd->width) * hd->height * 4);
    UnmapViewOfFile(head);
    if (!good) return CloseHandle(h), nullptr;
    g_base = static_cast<uint8_t*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, bytes));
    if (!g_base) return CloseHandle(h), nullptr;
    g_mapping = h, g_mapBytes = bytes;
    return reinterpret_cast<const Header*>(g_base);
}

// ---------------------------------------------------------------- the latched frame (view and picture together)
struct Frame {
    uint64_t frame = 0;
    uint32_t width = 0, height = 0;
    float position[3]{}, rotation[4]{0, 0, 0, 1}, fov = 1, root[3]{};
    std::vector<uint8_t> pixels;
};
std::mutex g_frameLock;
Frame g_frame;                       // what this CS2 frame shows (latched once per Present)
std::atomic<uint64_t> g_presents{0};  // Presents so far
std::atomic<bool> g_show{false};

// copy the newest finished slot (skipped if the engine rewrites it meanwhile)
bool Latch(const Header* hd) {
    const uint32_t slot = hd->latest.load(std::memory_order_acquire);
    if (slot >= kSlots) return false;
    const size_t pixels = size_t(hd->width) * hd->height * 4;
    const uint8_t* at = g_base + kHeader + slot * (kSlotHead + pixels);
    const auto* sh = reinterpret_cast<const SlotHead*>(at);
    const uint32_t s0 = sh->seq.load(std::memory_order_acquire);
    if (s0 & 1) return false;
    std::lock_guard lock(g_frameLock);
    if (sh->frame == g_frame.frame) return true;
    Frame f;
    f.frame = sh->frame, f.width = hd->width, f.height = hd->height;
    std::memcpy(f.position, sh->position, sizeof(f.position));
    std::memcpy(f.rotation, sh->rotation, sizeof(f.rotation));
    std::memcpy(f.root, sh->root, sizeof(f.root));
    f.fov = sh->fov;
    f.pixels.swap(g_frame.pixels);
    f.pixels.resize(pixels);
    std::memcpy(f.pixels.data(), at + kSlotHead, pixels);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (sh->seq.load(std::memory_order_relaxed) != s0) return g_frame.pixels.swap(f.pixels), false;
    g_frame = std::move(f);
    return true;
}

// park (metres, Y up) -> CS2 (units, Z up)
void ToCs(const float* p, float* out) {
    out[0] = float(p[0] / kMetresPerUnit), out[1] = float(-p[2] / kMetresPerUnit), out[2] = float(p[1] / kMetresPerUnit);
}
void Rotate(const float* q, const float* v, float* out) {
    const float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

// ---------------------------------------------------------------- drawing the skater over CS2 (Present)
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn g_present = nullptr;
void* g_presentAt = nullptr;
std::atomic<int> g_inHook{0};
std::atomic<bool> g_stopping{false};
std::atomic<uint32_t> g_bbW{0}, g_bbH{0};
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
bool g_ready = false, g_failed = false;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11BlendState* g_premult = nullptr;
ID3D11SamplerState* g_linear = nullptr;
ID3D11RasterizerState* g_noCull = nullptr;
ID3D11DepthStencilState* g_noDepth = nullptr;
ID3D11Texture2D* g_tex = nullptr;
ID3D11ShaderResourceView* g_view = nullptr;
uint32_t g_texW = 0, g_texH = 0;
uint64_t g_texFrame = ~0ull;

constexpr const char* kShader = R"(
Texture2D skater : register(t0);
SamplerState samp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(V i) : SV_Target { return skater.Sample(samp, i.uv); }  // premultiplied: the engine clears to transparent
)";

ID3DBlob* Compile(const char* entry, const char* target) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    D3DCompile(kShader, std::strlen(kShader), "cs2skate", nullptr, nullptr, entry, target, 0, 0, &code, &errors);
    Release(errors);
    return code;
}

bool Setup() {
    ID3DBlob *a = Compile("vs", "vs_5_0"), *b = Compile("ps", "ps_5_0");
    if (a) g_device->CreateVertexShader(a->GetBufferPointer(), a->GetBufferSize(), nullptr, &g_vs);
    if (b) g_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_ps);
    Release(a), Release(b);
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE, rt.SrcBlend = D3D11_BLEND_ONE, rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD, rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA, rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&bd, &g_premult);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sd, &g_linear);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
    g_device->CreateRasterizerState(&rd, &g_noCull);
    D3D11_DEPTH_STENCIL_DESC dd{};
    g_device->CreateDepthStencilState(&dd, &g_noDepth);
    return g_vs && g_ps && g_premult && g_linear && g_noCull && g_noDepth;
}

struct Saved {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp{};
    UINT vpCount = 1;
    ID3D11BlendState* blend = nullptr;
    float factor[4]{};
    UINT mask = 0;
    ID3D11DepthStencilState* depth = nullptr;
    UINT ref = 0;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    ID3D11InputLayout* layout = nullptr;
    explicit Saved(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(1, &rtv, &dsv);
        c->RSGetViewports(&vpCount, &vp);
        c->OMGetBlendState(&blend, factor, &mask);
        c->OMGetDepthStencilState(&depth, &ref);
        c->RSGetState(&raster);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->PSGetShaderResources(0, 1, &srv);
        c->PSGetSamplers(0, 1, &sampler);
        c->IAGetPrimitiveTopology(&topo);
        c->IAGetInputLayout(&layout);
    }
    void Restore(ID3D11DeviceContext* c) {
        c->OMSetRenderTargets(1, &rtv, dsv);
        c->RSSetViewports(vpCount, &vp);
        c->OMSetBlendState(blend, factor, mask);
        c->OMSetDepthStencilState(depth, ref);
        c->RSSetState(raster);
        c->VSSetShader(vs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->PSSetShaderResources(0, 1, &srv);
        c->PSSetSamplers(0, 1, &sampler);
        c->IASetPrimitiveTopology(topo);
        c->IASetInputLayout(layout);
        for (IUnknown* u : {static_cast<IUnknown*>(rtv), static_cast<IUnknown*>(dsv), static_cast<IUnknown*>(blend),
                            static_cast<IUnknown*>(depth), static_cast<IUnknown*>(raster), static_cast<IUnknown*>(vs),
                            static_cast<IUnknown*>(ps), static_cast<IUnknown*>(srv), static_cast<IUnknown*>(sampler),
                            static_cast<IUnknown*>(layout)})
            if (u) u->Release();
    }
};

void Draw(IDXGISwapChain* swap) {
    {
        std::lock_guard lock(g_frameLock);
        const Frame& f = g_frame;
        if (f.pixels.empty()) return;
        if (f.width != g_texW || f.height != g_texH) {
            Release(g_view), Release(g_tex);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = f.width, td.Height = f.height, td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM, td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE, td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_tex)) ||
                FAILED(g_device->CreateShaderResourceView(g_tex, nullptr, &g_view)))
                return Release(g_tex);
            g_texW = f.width, g_texH = f.height, g_texFrame = ~0ull;
        }
        if (f.frame != g_texFrame) {
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(g_context->Map(g_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
            const uint32_t row = f.width * 4;
            for (uint32_t y = 0; y < f.height; ++y)
                std::memcpy(static_cast<uint8_t*>(m.pData) + y * m.RowPitch, f.pixels.data() + size_t(y) * row, row);
            g_context->Unmap(g_tex, 0);
            g_texFrame = f.frame;
        }
    }
    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return;
    D3D11_TEXTURE2D_DESC bd;
    back->GetDesc(&bd);
    g_device->CreateRenderTargetView(back, nullptr, &rtv);
    Release(back);
    if (!rtv) return;
    Saved saved(g_context);
    const D3D11_VIEWPORT vp{0, 0, float(bd.Width), float(bd.Height), 0, 1};
    const float factor[4] = {};
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->RSSetViewports(1, &vp);
    g_context->OMSetBlendState(g_premult, factor, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(g_noDepth, 0);
    g_context->RSSetState(g_noCull);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_vs, nullptr, 0);
    g_context->PSSetShader(g_ps, nullptr, 0);
    g_context->PSSetShaderResources(0, 1, &g_view);
    g_context->PSSetSamplers(0, 1, &g_linear);
    g_context->Draw(3, 0);
    saved.Restore(g_context);
    Release(rtv);
}

HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* swap, UINT sync, UINT flags) {
    if (g_stopping.load()) return g_present(swap, sync, flags);
    ++g_inHook;
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(swap->GetDesc(&d))) g_bbW = d.BufferDesc.Width, g_bbH = d.BufferDesc.Height;
    if (!g_ready && !g_failed) {
        if (SUCCEEDED(swap->GetDevice(IID_PPV_ARGS(&g_device))) && g_device) {
            g_device->GetImmediateContext(&g_context);
            g_ready = Setup();
        }
        g_failed = !g_ready;
        if (g_failed) Say("the overlay could not be set up (D3D11)");
    }
    if (g_ready && g_show.load()) Draw(swap);
    ++g_presents;
    --g_inHook;
    return g_present(swap, sync, flags);
}

bool HookPresent() {
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;
    // Present from a throwaway swap chain's table (the same function the game's uses)
    WNDCLASSEXW wc{sizeof(WNDCLASSEXW), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr,
                   nullptr, nullptr, L"CS2SkateDummy", nullptr};
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
                                  wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1, sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM, sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = window, sd.SampleDesc.Count = 1, sd.Windowed = TRUE, sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap,
                                  &dev, nullptr, &ctx);
    void* present = swap ? (*reinterpret_cast<void***>(swap))[8] : nullptr;
    Release(swap), Release(dev), Release(ctx);
    if (window) DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (!present || MH_CreateHook(present, reinterpret_cast<void*>(&HkPresent), reinterpret_cast<void**>(&g_present)) != MH_OK)
        return false;
    if (MH_EnableHook(present) != MH_OK) return MH_RemoveHook(present), false;
    g_presentAt = present;
    return true;
}

// ---------------------------------------------------------------- state between frames
std::string g_exe, g_assets, g_maps;
std::string g_runningMap;
int g_runningOnline = 0;
uint32_t g_statusSeen = 0;
uint64_t g_latchedAt = ~0ull;
double g_startedAt = 0;
}  // namespace

extern "C" {
struct cs_in {
    double time;
    int32_t enabled, in_game;
    char map[64];
    int32_t full_res;
    int32_t online;  // 0 off, 1 host, 2 join
    char name[32];   // your CS2 name (the skater's name online)  // 1: the skater at CS2's resolution, 0: half
};
struct cs_out {
    int32_t active;     // CS2's view is the skate camera this frame
    float origin[3];    // CS2 view origin, angles (pitch, yaw, roll), fov (CS2's: horizontal at 4:3)
    float angles[3];
    float fov;
    float feet[3];      // the skater's feet in CS2 units
};

__declspec(dllexport) int cs_init(const char* exe, const char* assets, const char* maps) {
    g_exe = exe, g_assets = assets, g_maps = maps;
    g_stopping = false;
    // a wrong install (zips extracted into their own folders) otherwise just restarts a dying engine forever
    const std::string need[][2] = {{g_exe, "the engine (cs2skate-astral.zip)"},
                                   {g_assets + "\\private", "the Skate 3 assets (cs2skate-assets-part1..3.zip)"},
                                   {g_maps, "the parks (cs2skate-astral.zip)"}};
    for (const auto& [path, what] : need)
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            return Say("missing %s: %s not found. Extract every zip INTO astral\\lua (merge the cs2skate folders)",
                       what.c_str(), path.c_str()),
                   0;
    if (!HookPresent()) return Say("Present could not be hooked"), 0;
    return 1;
}

__declspec(dllexport) void cs_frame(const cs_in* in, cs_out* out) {
    std::memset(out, 0, sizeof(*out));
    const bool want = in->enabled && in->in_game;
    if (!want) {
        if (g_started) StopEngine(g_exe), g_runningMap.clear(), Say("skate engine stopped");
        g_show = false;
        return;
    }
    // (re)start the engine on this map's park, at the size of CS2's frame
    const uint32_t bw = g_bbW.load(), bh = g_bbH.load();
    if (!bw || !bh) return;
    const bool running = EngineRunning(g_exe);
    if (g_runningMap != in->map || in->online != g_runningOnline || !running) {
        if (!running && g_started && g_runningMap == in->map && in->time - g_startedAt > 5)
            Say("the skate engine stopped (crash? see cs2skate\\engine.log): restarting it"), g_started = false;
        if (!g_runningMap.empty() && g_runningMap == in->map && in->online == g_runningOnline && in->time - g_startedAt < 5)
            return;  // (just failed)
        const std::string park = g_maps + "\\" + in->map + "_link.skate";
        if (GetFileAttributesA(park.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (g_runningMap != in->map) Say("no park for %s (%s): convert it first", in->map, park.c_str());
            g_runningMap = in->map, g_startedAt = in->time;
            return;
        }
        const uint32_t scale = in->full_res ? 1 : 2;
        const uint32_t w = (bw / scale + 32) / 64 * 64, h = uint32_t(double(w) * bh / bw + 0.5);
        StartEngine(g_exe, g_assets, park, w, h, in->online, in->name);
        g_runningMap = in->map, g_runningOnline = in->online, g_startedAt = in->time, g_statusSeen = 0;
        return;
    }
    const Header* hd = Link();
    if (!hd) return;
    // the engine's online status line, when it changes
    if (const uint32_t seq = hd->status_seq.load(std::memory_order_acquire); seq != g_statusSeen) {
        char text[kStatusBytes];
        std::memcpy(text, reinterpret_cast<const char*>(hd) + kStatus, kStatusBytes);
        text[kStatusBytes - 1] = 0;
        if (hd->status_seq.load(std::memory_order_acquire) == seq) g_statusSeen = seq, Say("online: %s", text);
    }
    // one frame from the engine per CS2 frame: the first view call after a Present takes the newest
    if (g_presents.load() != g_latchedAt) Latch(hd), g_latchedAt = g_presents.load();
    std::lock_guard lock(g_frameLock);
    const Frame& f = g_frame;
    if (!f.frame) return;
    const float back[3] = {0, 0, -1};
    float fwd[3], cs[3];
    Rotate(f.rotation, back, fwd);
    const float fc[3] = {fwd[0], -fwd[2], fwd[1]};
    ToCs(f.position, out->origin);
    ToCs(f.root, out->feet);
    out->angles[0] = -std::asin(std::fmax(-1.0f, std::fmin(1.0f, fc[2]))) * 180 / kPi;
    out->angles[1] = std::atan2(fc[1], fc[0]) * 180 / kPi;
    out->fov = 2 * std::atan(std::tan(f.fov / 2) * 4.0f / 3.0f) * 180 / kPi;
    (void)cs;
    out->active = 1;
    g_show = true;
}

__declspec(dllexport) int cs_message(char* out, int size) {
    std::lock_guard lock(g_msgLock);
    if (g_messages.empty()) return 0;
    std::snprintf(out, size, "%s", g_messages.front().c_str());
    g_messages.pop_front();
    return 1;
}

__declspec(dllexport) void cs_unload() {
    g_show = false;
    g_stopping = true;
    if (g_presentAt) MH_DisableHook(g_presentAt), MH_RemoveHook(g_presentAt), g_presentAt = nullptr;
    while (g_inHook.load() > 0) Sleep(1);
    StopEngine(g_exe);
    for (IUnknown** p : {reinterpret_cast<IUnknown**>(&g_view), reinterpret_cast<IUnknown**>(&g_tex),
                         reinterpret_cast<IUnknown**>(&g_vs), reinterpret_cast<IUnknown**>(&g_ps),
                         reinterpret_cast<IUnknown**>(&g_premult), reinterpret_cast<IUnknown**>(&g_linear),
                         reinterpret_cast<IUnknown**>(&g_noCull), reinterpret_cast<IUnknown**>(&g_noDepth),
                         reinterpret_cast<IUnknown**>(&g_context), reinterpret_cast<IUnknown**>(&g_device)})
        if (*p) (*p)->Release(), *p = nullptr;
    g_ready = g_failed = false, g_texW = g_texH = 0;
    g_runningMap.clear();
    std::lock_guard lock(g_frameLock);
    g_frame = Frame{};
}
}
