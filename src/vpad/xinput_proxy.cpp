// xinput1_4.dll proxy: a virtual Xbox pad for skate3rust.exe (run it with SKATE3_INPUT=xinput). Put this next to the exe;
// it loads the real System32 xinput1_4.dll for everything else. The pad's state comes from shared memory
// Local\Skate3VirtualPad (pad.py, later the CS2 link): while `active`, slot 0 is the virtual pad merged with any real
// pad there (buttons OR'd, triggers max, a stick the virtual pad leaves centred is the real one's).
#include <windows.h>
#include <xinput.h>
#include <cstdint>

namespace {
struct Shared {
    uint32_t magic;   // 'VPAD'
    uint32_t active;  // 0: slot 0 is just the real pad
    uint32_t packet;  // bumped by the writer on every change
    XINPUT_GAMEPAD pad;
};
constexpr uint32_t kMagic = 0x44415056;

HMODULE g_real;
Shared* g_shared;

FARPROC Real(const char* name_or_ordinal) {
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        lstrcatW(path, L"\\xinput1_4.dll");
        g_real = LoadLibraryW(path);
    }
    return g_real ? GetProcAddress(g_real, name_or_ordinal) : nullptr;
}

Shared* Pad() {
    if (!g_shared) {
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared),
                                      L"Local\\Skate3VirtualPad");
        if (h) g_shared = static_cast<Shared*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    }
    return g_shared && g_shared->magic == kMagic && g_shared->active ? g_shared : nullptr;
}

#define ORD(n) reinterpret_cast<const char*>(static_cast<uintptr_t>(n))
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

DWORD Merge(DWORD user, XINPUT_STATE* out, GetStateFn real) {
    const DWORD r = real ? real(user, out) : ERROR_DEVICE_NOT_CONNECTED;
    const Shared* s = user == 0 ? Pad() : nullptr;
    if (!s) return r;
    if (r != ERROR_SUCCESS) ZeroMemory(out, sizeof(*out));
    XINPUT_GAMEPAD& g = out->Gamepad;
    const XINPUT_GAMEPAD v = s->pad;
    g.wButtons |= v.wButtons;
    g.bLeftTrigger = max(g.bLeftTrigger, v.bLeftTrigger);
    g.bRightTrigger = max(g.bRightTrigger, v.bRightTrigger);
    if (v.sThumbLX || v.sThumbLY) g.sThumbLX = v.sThumbLX, g.sThumbLY = v.sThumbLY;
    if (v.sThumbRX || v.sThumbRY) g.sThumbRX = v.sThumbRX, g.sThumbRY = v.sThumbRY;
    out->dwPacketNumber += s->packet;
    return ERROR_SUCCESS;
}

void FakeCaps(XINPUT_CAPABILITIES* c) {
    ZeroMemory(c, sizeof(*c));
    c->Type = XINPUT_DEVTYPE_GAMEPAD;
    c->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    c->Gamepad.wButtons = 0xF3FF;
    c->Gamepad.bLeftTrigger = c->Gamepad.bRightTrigger = 0xFF;
    c->Gamepad.sThumbLX = c->Gamepad.sThumbLY = c->Gamepad.sThumbRX = c->Gamepad.sThumbRY = -64;
    c->Vibration.wLeftMotorSpeed = c->Vibration.wRightMotorSpeed = 0xFF;
}

struct CapsEx {
    XINPUT_CAPABILITIES caps;
    WORD vendor, product, version, unknown1;
    DWORD unknown2;
};
}  // namespace

extern "C" {
DWORD WINAPI XInputGetState(DWORD user, XINPUT_STATE* out) {
    return Merge(user, out, reinterpret_cast<GetStateFn>(Real(ORD(2))));
}
DWORD WINAPI GetStateEx(DWORD user, XINPUT_STATE* out) {  // ordinal 100: also reports the guide button
    return Merge(user, out, reinterpret_cast<GetStateFn>(Real(ORD(100))));
}
DWORD WINAPI XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* c) {
    auto real = reinterpret_cast<DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*)>(Real(ORD(4)));
    const DWORD r = real ? real(user, flags, c) : ERROR_DEVICE_NOT_CONNECTED;
    if (r != ERROR_SUCCESS && user == 0 && Pad()) return FakeCaps(c), ERROR_SUCCESS;
    return r;
}
DWORD WINAPI GetCapabilitiesEx(DWORD a, DWORD user, DWORD flags, CapsEx* c) {  // ordinal 108 (SDL's shape)
    auto real = reinterpret_cast<DWORD(WINAPI*)(DWORD, DWORD, DWORD, CapsEx*)>(Real(ORD(108)));
    const DWORD r = real ? real(a, user, flags, c) : ERROR_DEVICE_NOT_CONNECTED;
    if (r != ERROR_SUCCESS && user == 0 && Pad()) {
        ZeroMemory(c, sizeof(*c));
        FakeCaps(&c->caps);
        c->vendor = 0x045E, c->product = 0x028E;  // an Xbox 360 pad
        return ERROR_SUCCESS;
    }
    return r;
}
DWORD WINAPI XInputSetState(DWORD user, XINPUT_VIBRATION* v) {
    auto real = reinterpret_cast<DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*)>(Real(ORD(3)));
    const DWORD r = real ? real(user, v) : ERROR_DEVICE_NOT_CONNECTED;
    return r != ERROR_SUCCESS && user == 0 && Pad() ? ERROR_SUCCESS : r;
}
void WINAPI XInputEnable(BOOL on) {
    if (auto real = reinterpret_cast<void(WINAPI*)(BOOL)>(Real(ORD(5)))) real(on);
}
DWORD WINAPI XInputGetAudioDeviceIds(DWORD user, LPWSTR render, UINT* render_n, LPWSTR capture, UINT* capture_n) {
    auto real = reinterpret_cast<DWORD(WINAPI*)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*)>(Real(ORD(10)));
    return real ? real(user, render, render_n, capture, capture_n) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputGetBatteryInformation(DWORD user, BYTE type, XINPUT_BATTERY_INFORMATION* b) {
    auto real = reinterpret_cast<DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*)>(Real(ORD(7)));
    const DWORD r = real ? real(user, type, b) : ERROR_DEVICE_NOT_CONNECTED;
    if (r != ERROR_SUCCESS && user == 0 && Pad()) {
        b->BatteryType = BATTERY_TYPE_WIRED, b->BatteryLevel = BATTERY_LEVEL_FULL;
        return ERROR_SUCCESS;
    }
    return r;
}
DWORD WINAPI XInputGetKeystroke(DWORD user, DWORD reserved, XINPUT_KEYSTROKE* k) {
    auto real = reinterpret_cast<DWORD(WINAPI*)(DWORD, DWORD, XINPUT_KEYSTROKE*)>(Real(ORD(8)));
    return real ? real(user, reserved, k) : ERROR_DEVICE_NOT_CONNECTED;
}
// the undocumented rest (guide-button wait, power off, bus info, ...): passed through, none takes more than 4 arguments
#define PASS(name, n)                                                                                     \
    DWORD WINAPI name(void* a, void* b, void* c, void* d) {                                               \
        auto real = reinterpret_cast<DWORD(WINAPI*)(void*, void*, void*, void*)>(Real(ORD(n)));           \
        return real ? real(a, b, c, d) : ERROR_DEVICE_NOT_CONNECTED;                                      \
    }
PASS(Ord101, 101)
PASS(Ord102, 102)
PASS(Ord103, 103)
PASS(Ord104, 104)
PASS(Ord109, 109)
}
