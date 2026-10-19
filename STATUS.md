# oras-coop — status (paused 2026-10-19)

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
| Walk/run animation on spawned trainers | ❌ legs don't move yet (see below) |
| Other trainer models (e.g. Brendan) | ❌ not tried — only the player's own model (always loaded) is safe |

## Where the animation work stopped

- `FUN_003FB1B4(mm, code, dir)` sets a character action. Re-calling it every frame resets the
  action, and code 1 is the NPC "stand" action — so that approach was removed.
- Lead: `FUN_003FF4D0` runs actions from the **movement-command queue** (cutscene "apply
  movement"): entry `+0x0A` code, `+0x0C` direction. Find the command table (walk/run per
  direction, step length), then queue step commands toward the friend's position instead of
  teleporting. Fallback: model animation vtable calls (+0xC0/+0x100/+0xD4) seen in `FUN_0072D500`.

## How to pick it up

1. Build (WSL, because Smart App Control blocks devkitARM on Windows):
   `bash /mnt/e/oras-coop/scripts/build.sh`
2. Install: copy `plugin/oras-coop.3gx` to
   `%APPDATA%\Azahar\sdmc\luma\plugins\000400000011C500\`
3. Server: `node E:\oras-coop\server\server.mjs` (port 7777). Fake friend for testing:
   `node E:\oras-coop\tools\fakefriend.mjs --map <id> --x <x> --y <y> --radius 45`
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

- Walk/run animation via movement commands (above).
- Calibrate overlay scale from real trainer positions so labels line up.
- Spawn the opposite-gender trainer model (needs the archive-load path in `FUN_003F663C`).
- Real 3DS hardware: `socInit` path and memory allocation are only tested on Azahar.
