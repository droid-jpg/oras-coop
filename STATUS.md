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
| Player-to-player collision | ✅ you can't walk through a friend's trainer; it stops blocking while it shares your tile (else you'd be trapped), tested with two real players |
| Two players on one PC | ✅ two Azahar windows on the relay: see each other, walk, map changes, collision, chat |
| Friend on a bike | ✅ their trainer rides the game's own Mach bike model (seated, turns with them); getting off swaps back to walking. Acro bike untested (same model id assumed) |
| Friend soaring / in a long cutscene | ✅ shown as "Name (busy)", standing at their takeoff spot; back to normal on landing |
| Friend in a battle | ✅ wild battle on Route 110: friends see "Name (busy)"; your own overlay hides during battles, dialogs and prompts |
| Friend surfing | ✅ their trainer sits on the surf Pokémon (the game's own surf mount); back to walking on land. Water marker only if the mount can't attach |
| Friend's own trainer (Brendan vs May) | ✅ both player models are loaded on every map (map-loader hook), so each friend shows as their own trainer from the first map on |
| Intro / title screen | ✅ nothing is spawned or drawn until the field is live (spawning during the intro used to hang the game) |
| Towns and routes | ✅ (a wrong "field is live" check had stopped syncing on routes; fixed) |
| Map names | ✅ player list shows where friends are ("Slateport City", "Route 109"), read from the game's own place-name text |
| Game menus and text boxes | ✅ the overlay steps aside while the X menu or a game text box is open (player mm+0x08 bits 0x8 / 0x02000000) |
| Doors and map edges | ✅ a friend leaving your map leaves a fading "Name -> Place" note where they left; arriving shows "Name arrived" |
| Talking to friends | ✅ walk up to a friend and press A: their greeting (menu → Your greeting) opens in the **game's own text box** (the game runs a tiny script of ours through its NPC-talk event; `Talk.cpp`), and they get "Name is talking to you!". Friends shown as markers only get a plugin-drawn box |

## Known bugs (found playing over Tailscale, 2026-10-19)

1. ~~**Talking to a friend crashes the game of the player who pressed A.**~~ **Fixed.** It was a hang, not a crash: the game's NPC-talk event ran script id 0 (our trainers have no script), which never resolves to a file, so the script runner looped forever. `Talk.cpp` hooks the run-script call (`FUN_003DC114`) and runs our message script for friends' trainers instead. Tested: 10+ talks in a row from both sides, no hang.
2. **Every friend shows as the same trainer (all Brendan or all May)** instead of their own
   choice. Each player's model is sent in STATE (`model` u16 @38) and the map-loader hook loads
   both (`Models.cpp`), so check where the puppet's model id is picked (`Puppet.cpp`): it may be
   taking our own model / the first resident one instead of `Remote::model`, or spawning before
   the friend's first STATE arrives and never respawning when `model` changes.

## Notes for the next session

- Everything above is verified in Azahar with the fake friend (and a real running player for
  the run clip), and with two Azahar windows on one PC. Over the internet via Tailscale it
  connects and syncs (2026-10-19), but see Known bugs above.
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

- **Polish: animations during cutscenes.** While a friend is in a cutscene (busy), their trainer
  stops animating and just slides around. Keep driving walk/run (or freeze position) instead.
- **Spectate a friend's battle.** Pressing A on a friend who is in a battle opens a popup above
  them with their Pokémon sprites, active Pokémon, HP, etc. Needs a new frame type carrying the
  battle state, sent while `status` busy is from a battle.
- Trading / battles between players (large: needs the game's link-trade/battle protocols).
- Show friends on other maps' doors/warps (e.g. "Bot is inside"), and a /who list in chat.
