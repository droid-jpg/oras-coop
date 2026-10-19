# oras-coop — status (updated 2026-10-19)

Co-op mod for **Pokémon Alpha Sapphire v1.4** on Azahar: a 3GX plugin plus a Node relay server.
Reverse-engineering notes live in `E:\ORAS-RE\NOTES.md` (addresses, structs, function list).

## What works

| Feature | State |
|---|---|
| Relay server (`server/server.mjs`) | ✅ rooms, IDs stamped server-side, STATE/CHAT/JOIN/LEAVE, timeouts |
| Plugin connects, status HUD, chat, player list, radar | ✅ tested in Azahar |
| Name labels / off-screen arrows | ✅ projected through the game's own camera — sit right above heads, indoors and out |
| Finding the player | ✅ player state object 0x08C6E880 (stable), incremental fallback scans (no frame hitches) |
| Smooth movement | ✅ friends are shown 120 ms behind, interpolated between packets (sender clock in STATE @40) |
| **Real models** (Display → Real models) | ✅ spawns a trainer per friend with the game's own spawn fn, drives position + facing every frame, despawns cleanly |
| Walk/run animation on spawned trainers | ✅ walk 0x10 / run 0x19 (verified with a real running player), stand when stopped |
| Player-to-player collision | ✅ you can't walk through a friend's trainer (its collision tile follows it) |
| Friend's own trainer (Brendan vs May) | ✅ the map loader is hooked to load their model on every map change; a friend who joins mid-map shows as the rival model (Littleroot) or yours until the next map change |
| Intro / title screen | ✅ nothing is spawned or drawn until the field is live (spawning during the intro used to hang the game). Battles not tested yet |

## Notes for the next session

- Everything above is verified in Azahar with the fake friend (and a real running player for
  the run clip). Not yet tried with two real Azahar instances over the internet.
- Details for every address/function are in `re/NOTES.md` (action table, anim byte mm+0x58,
  grid records, resident model table + list-loader hook, camera matrix, field-active flag).
- The model hook only knows Brendan/May; other outfits/characters would need their ids added.

## How to pick it up

1. Build (WSL, because Smart App Control blocks devkitARM on Windows):
   `bash /mnt/e/oras-coop/scripts/build.sh`
2. Install: copy `plugin/oras-coop.3gx` to
   `%APPDATA%\Azahar\sdmc\luma\plugins\000400000011C500\`
3. Server: `node E:\oras-coop\server\server.mjs` (port 7777). Fake friend for testing:
   `node E:\oras-coop\tools\fakefriend.mjs --map <id> --x <x> --y <y> --radius 30 --speed 0.15 --stopgo`
   (`--anim 0x19` run clip, `--model 0xab` May, `--x2/--y2 --after N` jump to test collision)
4. Memory tools (Azahar GDB stub, port 24689): `node E:\ORAS-RE\tools\mem.mjs`
   — `watch`, `read`, `dump`, `find-pos`, `find-u32`, `write-float/u32`, `catch-write` (`SET=` to
   write first), `catch-break`, `append-model`.
5. Ghidra project: `E:\ORAS-RE\ghidra\ORAS_AS_1_4.gpr` (code.bin + DllField block);
   headless scripts in `E:\ORAS-RE\ghidra_scripts` (Decomp, Listing, FindStores, ListFuncs…).

## Current machine state

- Azahar settings changed: plugin loader **on**, GDB stub **on** (backup: `qt-config.ini.bak-oras-coop`).
  Turn the GDB stub off for normal play — it slows things when a debugger is attached.
- Plugin config `oras-coop.txt` has `puppets=1` (Real models on).
- Save file untouched (last written 2026-10-18 22:46, before any automation).
- Plugin menu (SELECT) also has two debug entries: spawn test character / action on test character.

## Ideas queue

- Try two real players over the internet (relay on a public host or Tailscale).
- Trading / battles between players (large: needs the game's link-trade/battle protocols).
- Show friends on other maps' doors/warps (e.g. "Bot is inside"), and a /who list in chat.
