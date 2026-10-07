"""Writes lua/cs2skate.lua: CS2Craft's tested DLL loader (kernel32 via Astral exports, manual-map fallback) + the
skate script. Run: python make_lua.py <cs2craft.lua> (the loader comes from it)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
src = open(sys.argv[1], encoding="utf-8").read()
loader = src[src.index("local ffi = require"):src.index("-- ── the DLL")].rstrip().replace("[cs2craft]", "[cs2skate]")

head = r'''-- cs2skate.lua: skate in CS2. cs2skate.dll (next to this script, in astral\lua) runs the Skate 3 Rust Engine hidden on
-- a park made from this map's collision; your controller skates, CS2's camera follows the engine's and the skater is
-- drawn over CS2. This script loads the DLL, keeps the menu, hands it the map each frame, puts CS2's view on the
-- skate camera, hides your CS2 player (it stays put) and keeps your keys off it.
'''

tail = r'''

-- ── the DLL ──────────────────────────────────────────────────────────────────────────────────────────────
pcall(ffi.cdef, [[
    typedef struct {
        double time;
        int32_t enabled, in_game;
        char map[64];
        int32_t full_res;
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
'''
out = os.path.join(HERE, "lua", "cs2skate.lua")
os.makedirs(os.path.dirname(out), exist_ok=True)
open(out, "w", encoding="utf-8", newline="\n").write(head + loader + tail)
print("ok", out)
