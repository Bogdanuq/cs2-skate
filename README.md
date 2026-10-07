# 🛹 Skate 3 in CS2

Skate with your controller inside Counter-Strike 2: the [Skate 3 Rust Engine](https://github.com/SK8-ENGINE/skate-3-rust-engine)
runs hidden in the background on a skatepark built from the CS2 map you're on. Your controller drives the skater, CS2's
camera follows the skate camera and the skater is drawn into your game. Your CS2 player, gun and HUD are hidden while
you skate and come back when you stop.

## ✅ Requirements

- Counter-Strike 2 + **Astral** (Lua scripting)
- An **Xbox-style controller** (XInput). PlayStation controllers are **not** seen.
- A GPU with **Vulkan** drivers (any recent NVIDIA/AMD/Intel driver)
- The [Microsoft Visual C++ Redistributable x64](https://aka.ms/vs/17/release/vc_redist.x64.exe) (most PCs already
  have it; Steam games install it)

The engine comes **already built** in `cs2skate-astral.zip`: nothing to compile. The CS2 script starts it by itself
in the background when you tick Skate.
- About **5 GB** of free disk space

## 📥 Install

1. Download every zip from the [latest release](../../releases/latest):
   - `cs2skate-astral.zip`: the script, its DLL, the engine and the parks
   - `cs2skate-assets-part1.zip`, `cs2skate-assets-part2.zip`, ...: the Skate 3 files
2. Extract **all of them** into your Astral lua folder (say yes to merging folders):
   ```
   ...\steamapps\common\Counter-Strike Global Offensive\astral\lua
   ```

It should look like this:

```
astral\lua\
├─ cs2skate.dll
├─ cs2skate.lua
└─ cs2skate\
   ├─ skate3rust.exe
   ├─ xinput1_4.dll
   ├─ bevy_dylib-....dll
   ├─ std-....dll
   ├─ manifest.json
   ├─ mods\
   ├─ steam-relay        (online)
   ├─ maps\
   │  ├─ de_dust2_link.skate
   │  └─ de_mirage_link.skate
   └─ assets\
      └─ private\...
```

## 🎮 How to use

1. Start a **local game on de_dust2 or de_mirage** (Practice with bots, or your own server).
2. Open the console and type `sv_cheats 1`.
3. Load `cs2skate.lua` in Astral.
4. Go to **Scripting → Script elements** and tick **Skate**.
5. Wait a few seconds while the engine starts, then grab your controller and go!
6. Untick **Skate** to stop. Your player, gun and HUD come back.

**Skater resolution**: *Half* (lighter, default) or *Full* (sharper skater, more GPU).

## 🌐 Online (skate with friends)

Up to **10 skaters** through Steam, on the same map. You don't need to be in the same CS2 server: everyone stays in
their own local game.

1. Everyone loads the **same map** (dust2 or mirage) with `sv_cheats 1`.
2. One person sets **Online → Host**, everyone else **Online → Join**, then ticks **Skate**.
3. The Astral console says `[cs2skate] online: hosting (N skaters here)` / `joined (N skaters here)`.

Joiners keep looking until a lobby on that map shows up, so the order doesn't matter much. Your CS2 name is your
skater's name. Steam has to be running (it is, with CS2). Changing **Online** restarts the skate engine.

## 🕹️ Controls

| Button | Action |
|---|---|
| **A** | push |
| **Left stick** | steer / carve |
| **Right stick** | flick tricks (pull down then flick up to ollie) |
| **Triggers** | grabs |

## 🔧 Troubleshooting

- **Nothing happens when I tick Skate**: check the Astral console for `[cs2skate]` lines. Make sure
  `skate3rust.exe` is directly inside `astral\lua\cs2skate` and `assets` contains a `private` folder.
- **Controller does nothing**: use an Xbox-style controller, connect it before ticking Skate, re-tick Skate.
- **"no park for &lt;map&gt;"**: parks so far: **de_dust2** and **de_mirage**. Other maps need their own
  `<map>_link.skate` in `astral\lua\cs2skate\maps` (made with `src/maps/cs2_to_skate.py`).
- **I can still see my CS2 player or HUD**: `sv_cheats 1` **before** ticking Skate, then re-tick it.
- **Bots keep shooting my parked player**: `bot_stop 1` (or `bot_kick`).
- **Skater looks blurry**: Skater resolution → *Full*.
- **Game stutters**: both games share your GPU; keep Skater resolution on *Half*.

## ⚠️ Notes

- Local games only (needs `sv_cheats 1`).
- The skater is drawn on top of everything, including walls in front of it.
- The engine's own menus (pause) aren't shown in CS2; the trick score is.

## 🧰 Source (`src/`)

| Folder | What |
|---|---|
| `cs2/` | `cs2skate.dll` (Present-hook overlay, starts the engine through Explorer, camera link) + `make_lua.py` → `lua/cs2skate.lua` |
| `engine-patch/` | `cs2_link.patch`: the CS2 link for skate-3-rust-engine at `af56404` (GPL-3) |
| `vpad/` | `xinput1_4.dll` proxy (gives the engine the controller, plus a virtual pad for testing) and `pad.py` |
| `maps/` | `cs2_to_skate.py`: CS2 map collision (`map_collision.py` → `.cs2col`) → `.skate` park |

Build the DLL: `cmake -S src/cs2 -B build -A x64 && cmake --build build --config Release`.
Build the engine: apply the patch to skate-3-rust-engine, then `cargo build -p skate-game --bin skate3rust`.

## ❤️ Credits

- [SK8-ENGINE / Skate 3 Rust Engine](https://github.com/SK8-ENGINE/skate-3-rust-engine) (GPL-3)
- Skate 3 © Electronic Arts
