-- cs2skate.lua: skate in CS2. cs2skate.dll (next to this script, in astral\lua) runs the Skate 3 Rust Engine hidden on
-- a park made from this map's collision; your controller skates, CS2's camera follows the engine's and the skater is
-- drawn over CS2. This script loads the DLL, keeps the menu, hands it the map each frame, puts CS2's view on the
-- skate camera, hides your CS2 player (it stays put) and keeps your keys off it.
local ffi = require("ffi")

-- ── kernel32 (LoadLibrary and friends) through Astral's export lookup, as the other DLL scripts do ──────
local K = {}
do
    local m = type(memory) == "table" and memory or (type(utils) == "table" and utils or nil)
    local ok_mem = m and m.get_module_export
    if ok_mem then
        local function e(mod, name, sig)
            local ok, a = pcall(m.get_module_export, mod, name)
            return (ok and a and a ~= 0) and ffi.cast(sig, a) or nil
        end
        for name, sig in pairs({
            LoadLibraryA       = "void*(*)(const char*)",
            FreeLibrary        = "int(*)(void*)",
            GetModuleHandleA   = "void*(*)(const char*)",
            GetProcAddress     = "void*(*)(void*, const char*)",
            GetModuleFileNameA = "unsigned long(*)(void*, char*, unsigned long)",
            VirtualAlloc       = "void*(*)(void*, size_t, unsigned long, unsigned long)",
            VirtualFree        = "int(*)(void*, size_t, unsigned long)",
            CreateFileA        = "void*(*)(const char*, unsigned long, unsigned long, void*, unsigned long, unsigned long, void*)",
            ReadFile           = "int(*)(void*, void*, unsigned long, unsigned long*, void*)",
            GetFileSize        = "unsigned long(*)(void*, unsigned long*)",
            CloseHandle        = "int(*)(void*)",
        }) do K[name] = e("kernel32.dll", name, sig) end
        K.RtlAddFunctionTable = e("ntdll.dll", "RtlAddFunctionTable", "unsigned char(*)(void*, unsigned long, uint64_t)")
        ok_mem = K.LoadLibraryA and K.GetModuleHandleA and K.GetProcAddress and K.GetModuleFileNameA and K.FreeLibrary
    end
    if not ok_mem then
        for _, d in ipairs({
            "void* LoadLibraryA(const char* path);",
            "int FreeLibrary(void* module);",
            "void* GetModuleHandleA(const char* name);",
            "void* GetProcAddress(void* module, const char* name);",
            "unsigned long GetModuleFileNameA(void* module, char* path, unsigned long size);",
        }) do pcall(ffi.cdef, d) end
        if not pcall(function() return ffi.C.LoadLibraryA end) then
            return print("[cs2skate] can't load: launch CS2 with -insecure or update Astral")
        end
        K = ffi.C
    end
end

local function log(fmt, ...) print("[cs2skate] " .. string.format(fmt, ...)) end

-- ── PE manual mapper (when LoadLibraryA is blocked for unsigned DLLs), from the emotes and shaders scripts ──
local function manual_map(filepath)
    local pe
    do
        local ok, f = pcall(function() return io.open(filepath, "rb") end)
        if ok and f then pe = f:read("*a"); f:close()
        elseif K.CreateFileA then
            local h = K.CreateFileA(filepath, 0x80000000, 1, nil, 3, 0, nil)
            if tonumber(ffi.cast("intptr_t", h)) == -1 then return nil end
            local sz = tonumber(K.GetFileSize(h, nil))
            if sz <= 0 then K.CloseHandle(h); return nil end
            local buf = ffi.new("uint8_t[?]", sz)
            local nr = ffi.new("unsigned long[1]")
            K.ReadFile(h, buf, sz, nr, nil); K.CloseHandle(h)
            pe = ffi.string(buf, sz)
        end
    end
    if not pe or #pe < 256 or not K.VirtualAlloc then return nil end

    local function u16(o) return pe:byte(o+1) + pe:byte(o+2) * 256 end
    local function u32(o) return u16(o) + u16(o+2) * 65536 end
    local function u64(o) return u32(o) + u32(o+4) * 4294967296 end
    if u16(0) ~= 0x5A4D then return nil end
    local nt = u32(0x3C)
    if u32(nt) ~= 0x4550 or u16(nt+24) ~= 0x020B then return nil end

    local nsec, opt = u16(nt+6), nt+24
    local ep, ib = u32(opt+16), u64(opt+24)
    local isz, hsz = u32(opt+56), u32(opt+60)
    local ndd = u32(opt+108)
    local imp = ndd > 1 and u32(opt+120) or 0
    local rel, rsz = ndd > 5 and u32(opt+152) or 0, ndd > 5 and u32(opt+156) or 0
    local exp = ndd > 0 and u32(opt+112) or 0
    local exc, esz = ndd > 3 and u32(opt+136) or 0, ndd > 3 and u32(opt+140) or 0

    local base = K.VirtualAlloc(nil, isz, 0x3000, 0x40)
    if base == nil then return nil end
    local bp = ffi.cast("uint8_t*", base)
    local bn = tonumber(ffi.cast("uintptr_t", base))
    ffi.fill(bp, isz, 0)
    ffi.copy(bp, pe, math.min(hsz, #pe))
    local sh = nt + 24 + u16(nt+20)
    for i = 0, nsec-1 do
        local s = sh + i*40
        local va, rs, rp = u32(s+12), u32(s+16), u32(s+20)
        if rs > 0 and rp > 0 and rp+rs <= #pe then ffi.copy(bp+va, pe:sub(rp+1, rp+rs), rs) end
    end

    local function m16(o) return ffi.cast("uint16_t*", bp+o)[0] end
    local function m32(o) return tonumber(ffi.cast("uint32_t*", bp+o)[0]) end
    local function m64(o) return ffi.cast("uint64_t*", bp+o)[0] end
    local function w64(o, v) ffi.cast("uint64_t*", bp+o)[0] = v end
    local function bail() K.VirtualFree(base, 0, 0x8000); return nil end

    local delta = ffi.cast("int64_t", bn - ib)
    if bn ~= ib and rel > 0 then
        local p = rel
        while p < rel + rsz do
            local pg, bsz = m32(p), m32(p+4)
            if bsz < 8 then break end
            for j = 0, (bsz-8)/2 - 1 do
                local e = tonumber(m16(p + 8 + j*2))
                if math.floor(e / 4096) == 10 then
                    local off = pg + e % 4096
                    w64(off, m64(off) + delta)
                end
            end
            p = p + bsz
        end
    end

    if imp > 0 then
        local id = imp
        while m32(id+12) ~= 0 do
            local dn = ffi.string(bp + m32(id+12))
            local dll = K.GetModuleHandleA(dn)
            if dll == nil and K.LoadLibraryA then dll = K.LoadLibraryA(dn) end
            if dll == nil then return bail() end
            local ilt = m32(id) ~= 0 and m32(id) or m32(id+16)
            local iat = m32(id+16)
            local i = 0
            while m64(ilt + i*8) ~= 0ULL do
                local th = m64(ilt + i*8)
                local fn
                if ffi.cast("int64_t", th) < 0 then
                    fn = K.GetProcAddress(dll, ffi.cast("const char*", ffi.cast("uintptr_t", tonumber(ffi.cast("uint16_t", th)))))
                else
                    fn = K.GetProcAddress(dll, ffi.string(bp + tonumber(ffi.cast("uint32_t", th)) + 2))
                end
                if fn == nil then return bail() end
                ffi.cast("void**", bp + iat + i*8)[0] = fn
                i = i + 1
            end
            id = id + 20
        end
    end

    if exc > 0 and esz > 0 and K.RtlAddFunctionTable then
        K.RtlAddFunctionTable(bp + exc, math.floor(esz / 12), ffi.cast("uint64_t", bn))
    end
    if ep ~= 0 then
        local ok2 = pcall(ffi.cast("int(*)(void*, unsigned long, void*)", bp + ep), base, 1, nil)
        if not ok2 then return bail() end
    end

    return {
        base = base,
        get_export = function(name)
            if exp == 0 then return nil end
            local nn, fa, na, oa = m32(exp+24), m32(exp+28), m32(exp+32), m32(exp+36)
            for i = 0, nn-1 do
                if ffi.string(bp + m32(na + i*4)) == name then
                    return bp + m32(fa + tonumber(m16(oa + i*2)) * 4)
                end
            end
            return nil
        end,
    }
end

local function load_module(filepath)
    local h = K.LoadLibraryA(filepath)
    if h ~= nil then
        return {get_proc = function(n) return K.GetProcAddress(h, n) end, free = function() K.FreeLibrary(h) end}
    end
    local mm = manual_map(filepath)
    if mm then return {get_proc = mm.get_export, free = function() end, mapped = true} end
end

-- ── the DLL ──────────────────────────────────────────────────────────────────────────────────────────────
pcall(ffi.cdef, [[
    typedef struct {
        double time;
        int32_t enabled, in_game;
        char map[64];
        int32_t full_res;
        int32_t online;
        char name[32];
    } cs2skate_in_t;
    typedef struct {
        int32_t active;
        float origin[3];
        float angles[3];
        float fov;
        float feet[3];
    } cs2skate_out_t;
]])
local buffer = ffi.new("char[1024]")
local length = tonumber(K.GetModuleFileNameA(nil, buffer, 1024))
local root = ffi.string(buffer, length):match("^(.*)\\game\\bin\\win64\\[^\\]+$")
if not root then return log("can't find the game folder") end
local lua_dir = root .. "\\astral\\lua"
-- everything else is in astral\lua\cs2skate: the engine, maps\<map>_link.skate and assets (the Skate 3 files the
-- engine's setup converted from your copy of the game)
local SKATE = lua_dir .. "\\cs2skate"
local ENGINE = SKATE .. "\\skate3rust.exe"
local ASSETS = SKATE .. "\\assets"
local MAPS = SKATE .. "\\maps"
local mod = load_module(lua_dir .. "\\cs2skate.dll")
if not mod then return log("could not load %s\\cs2skate.dll: is it there?", lua_dir) end
local api = {}
for name, sig in pairs({init = "int(*)(const char*, const char*, const char*)",
                        frame = "void(*)(const cs2skate_in_t*, cs2skate_out_t*)",
                        message = "int(*)(char*, int)", unload = "void(*)(void)"}) do
    local p = mod.get_proc("cs_" .. name)
    if p == nil then mod.free(); return log("cs2skate.dll is an old build without cs_%s: copy the new one", name) end
    api[name] = ffi.cast(sig, p)
end
local fin, fout, msg = ffi.new("cs2skate_in_t"), ffi.new("cs2skate_out_t"), ffi.new("char[512]")
local function messages() while api.message(msg, 512) == 1 do print("[cs2skate] " .. ffi.string(msg)) end end
local ready = api.init(ENGINE, ASSETS, MAPS) == 1
messages()
if not ready then api.unload(); mod.free(); return end

-- ── the menu ─────────────────────────────────────────────────────────────────────────────────────────────
local box = ui.find_container("Scripting, Script elements")
local ui_on = box:add_checkbox("Skate")
ui_on:add_tooltip("Skate 3 in CS2: your controller skates (the engine runs hidden; your own local game with "
    .. "sv_cheats 1). The map needs a park: astral\\lua\\cs2skate\\maps\\<map>_link.skate.")
local ui_res = box:add_dropdown("Skater resolution", {"Half", "Full"})
ui_res:add_tooltip("The skater drawn at half CS2's resolution (lighter: both games share your GPU) or full.")
local ui_online = box:add_dropdown("Online", {"Off", "Host", "Join"})
ui_online:add_tooltip("Skate with others (Steam, up to 10, same map): one hosts, the rest join. You don't need to "
    .. "be in the same CS2 server. Changing this restarts the skate engine.")

-- ── each frame ───────────────────────────────────────────────────────────────────────────────────────────
local active, hidden = false, false

-- your CS2 player stays where it is, not drawn (EF_NODRAW 32: no model, no shadow), with its gun and HUD hidden too
-- (ent_fire and r_drawviewmodel need sv_cheats 1)
local function hide(on)
    if on == hidden then return end
    hidden = on
    utils.console_exec(on and "sv_cheats 1; r_drawviewmodel 0; cl_drawhud 0; ent_fire !self addeffects 32"
        or "r_drawviewmodel 1; cl_drawhud 1; ent_fire !self removeeffects 32")
end

callbacks.add("view", function(v)
    local lp = entities.get_local_player()
    local ok_map, map = pcall(globals.get_map_name_short)
    fin.time = globals.get_real_time()
    fin.enabled = ui_on:get() and 1 or 0
    fin.in_game = (lp ~= nil and ok_map and type(map) == "string" and #map > 0) and 1 or 0
    ffi.copy(fin.map, fin.in_game == 1 and map:sub(1, 63) or "")
    fin.full_res = ui_res:get() == "Full" and 1 or 0
    local online = ui_online:get()
    fin.online = online == "Host" and 1 or online == "Join" and 2 or 0
    local ok_name, name = pcall(function() return lp and lp:get_name() end)
    ffi.copy(fin.name, (ok_name and type(name) == "string" and name or ""):sub(1, 31))
    api.frame(fin, fout)
    messages()
    active = fout.active == 1
    if active then
        v.origin = vector(fout.origin[0], fout.origin[1], fout.origin[2])
        pcall(function() v.angles = qangle(fout.angles[0], fout.angles[1], fout.angles[2]) end)
        pcall(function() v.fov = fout.fov end)
    end
    hide(active)
end)

-- your keys don't move the CS2 player while skating
callbacks.add("move", function(cmd)
    if not active then return end
    cmd.forward_move, cmd.left_move = 0, 0
    for _, b in ipairs({"in_attack", "in_attack2", "in_jump", "in_duck", "in_use", "in_reload", "in_sprint",
                        "in_forward", "in_back", "in_moveleft", "in_moveright"}) do
        pcall(function() cmd[b] = false end)
    end
end)

callbacks.add("shutdown", function()
    hide(false)
    api.unload()
    mod.free()
end)
