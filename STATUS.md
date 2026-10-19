# oras-coop — status (updated 2026-10-19)

Co-op mod for **Pokémon Alpha Sapphire v1.4** on Azahar: a 3GX plugin plus a Node relay server.
Reverse-engineering notes live in `E:\ORAS-RE\NOTES.md` (addresses, structs, function list).

## What works

| Feature | State |
|---|---|
| Relay server (`server/server.mjs`) | ✅ rooms, IDs stamped server-side, STATE/CHAT/JOIN/LEAVE, timeouts |
| Plugin connects, status HUD, chat, player list, radar | ✅ tested in Azahar |
| Overlay markers / off-screen arrows | ✅ (scale is a guess — labels sit a bit off) |
| Finding the player | ✅ player state object 0x08C6E880 (stable), vtable fallback scan |
| **Real models** (Display → Real models) | ✅ spawns a trainer per friend with the game's own spawn fn, drives position + facing every frame, despawns cleanly |
| Walk/run animation on spawned trainers | ✅ legs walk while moving, stand when stopped; sender's own clip forwarded (run = 0x19), speed fallback |
| Player-to-player collision | ✅ you can't walk through a friend's trainer (its collision tile follows it) |
| Friend's own trainer (Brendan vs May) | 🟡 used only where the map already has that model loaded; otherwise shown with your model |

## Where the model work stopped

- Animation and collision are done (details in `re/NOTES.md`: action table, anim byte mm+0x58,
  grid records mm+0x64/0x90/0xBC).
- Opposite-gender model: adding a descriptor to the resident table makes the character spawn
  but invisible — the 3D data is preloaded per map by the model-set system (`FUN_00402FCC`).
  Next step: get the model into that map list before load (hook), or find its per-model loader.
- Running was verified by sending clip 0x19 from the fake friend; a real running player
  (B + circle pad) hasn't been captured yet, so check the forwarded id with two real players.

## How to pick it up

1. Build (WSL, because Smart App Control blocks devkitARM on Windows):
   `bash /mnt/e/oras-coop/scripts/build.sh`
2. Install: copy `plugin/oras-coop.3gx` to
   `%APPDATA%\Azahar\sdmc\luma\plugins\000400000011C500\`
3. Server: `node E:\oras-coop\server\server.mjs` (port 7777). Fake friend for testing:
   `node E:\oras-coop\tools\fakefriend.mjs --map <id> --x <x> --y <y> --radius 30 --speed 0.15 --stopgo`
   (`--anim 0x19` run clip, `--model 0xab` May, `--x2/--y2 --after N` jump to test collision)
4. Memory tools (Azahar GDB stub, port 24689): `node E:\ORAS-RE\tools\mem.mjs`
   — `watch`, `read`, `dump`, `find-pos`, `find-u32`, `write-float/u32`, `catch-write`, `catch-break`.
5. Ghidra project: `E:\ORAS-RE\ghidra\ORAS_AS_1_4.gpr` (code.bin + DllField block);
   headless scripts in `E:\ORAS-RE\ghidra_scripts` (Decomp, Listing, FindStores, ListFuncs…).

## Current machine state

- Azahar settings changed: plugin loader **on**, GDB stub **on** (backup: `qt-config.ini.bak-oras-coop`).
  Turn the GDB stub off for normal play — it slows things when a debugger is attached.
- Plugin config `oras-coop.txt` has `puppets=1` (Real models on).
- Save file untouched (last written 2026-10-18 22:46, before any automation).
- Plugin menu (SELECT) also has two debug entries: spawn test character / action on test character.

## Ideas queue

- Opposite-gender trainer model (see above).
- Calibrate overlay scale from real trainer positions so labels line up (labels sit high/left).
- Real 3DS hardware: `socInit` path and memory allocation are only tested on Azahar.
