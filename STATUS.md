# oras-mmo — status (updated 2026-10-20)

Co-op mod for **Pokémon Alpha Sapphire v1.4** on Azahar: a 3GX plugin plus a Node relay server.
Reverse-engineering notes live in `E:\ORAS-RE\NOTES.md` (addresses, structs, function list).

## What works

| Feature | State |
|---|---|
| Relay server (`server/server.mjs`) | ✅ rooms, IDs stamped server-side, STATE/CHAT/JOIN/LEAVE/BATTLE/TRADE, timeouts |
| Plugin connects, status HUD, chat, player list, radar | ✅ tested in Azahar |
| Overlay keeps Azahar's upscaling | ✅ the overlay is drawn by the 3DS GPU inside the game's frame (Gpu.cpp), both screens, so the game stays at Azahar's internal resolution (CTRPF's CPU drawing dropped it to native). Display → Sharp overlay (GPU) switches back to CPU drawing. Tested: title, intro, field, wild battles, X menu, map changes, notices/labels/arrows with a fake friend. Right eye (3D on) not tested |
| `/who` | ✅ "/who" in Send chat (typed, or the last preset) isn't sent: it lists who's online and where as notices ("3 online in HOENN: you and 2 friends", then "Bot: here", "Ann: Granite Cave (busy)"...) |
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
| "Name is inside" on doors and warps | ✅ every map change seen (yours, and every friend's on any map) teaches a warp: map A at spot P leads to B, and B's arrival spot leads back. A friend on another map gets a steady label on the door/edge of your map that leads toward them (up to 4 doors deep): "Ann is inside" (same place name: a building or another room of this cave) or "Cal, Dee are in Littleroot Town". Fly, Teleport, Dig, Escape Rope, soaring and whiteouts aren't warps: the mover flags them (STATE status bit 3, set for 5 s after a menu, 2 s+ of script control or 3 s+ off the field). Kept in memory for the session. Tested with fake friends walking through a "door" (and one flagged as flying: no label) |
| Friends in other outfits / models | ✅ any field model a friend's game reports, not just Brendan/May: the map-load hook adds it too, with the descriptor read from the game's own archive (`FUN_0019C678` + `FUN_00128E4C`) on the plain field and remembered in `oras-mmo-models.bin` next to the plugin, so it's there from the first map after Continue. Status shows "Models: 13C (read) loaded, AB loaded". Tested with a fake friend sending model 0x13C (a cyclist): stand-in until the next map load, then their own model. Note: vanilla ORAS only ever uses 0xAB/0xAC for the player; this covers modded games and future outfits |
| Notices | ✅ "Name joined!", "Name left.", chat and "Name is talking to you!" show one at a time in a small box styled like the game's text box at the top of the screen (3.5 s each). They never take control from you (tested: walking straight through join, chat and leave). Notices that arrive during a battle, menu or text box wait until it's over |
| Joins/leaves during battles and cutscenes | ✅ nothing is spawned or deleted while DllField is unloaded (battles) or while a script, text box or menu is up; it happens once that ends. Tested: three friends joining and leaving on your map mid-battle, and a friend leaving while you were in a text box with them |
| Busy friends keep animating | ✅ a friend walking under script control (cutscene) moves its legs instead of sliding |
| Watching a friend's battle | ✅ press A on a friend in a battle: the game's text box shows what each side has out with HP, and the bottom screen lists both teams with live HP bars. Read from DllBattle's memory (`Battle.cpp`), sent as BATTLE frames. Tested on a real wild battle and a fake one |
| Trading | ✅ all by talking, in the game's own text box: A on a friend → Trade in the choice list → the game's choice list of your party → offer sent. X talks to you → "X offers ..." Yes → picks one back (that also accepts). You talk to X again → "You give / You get … Trade with X?" Yes. The server confirms both accepts to both games at once (COMMIT), then each swaps in the other Pokémon once back on the plain field. B backs out of the list; No calls a trade off. Not the game's link trade: no trade evolutions, no Pokédex entry. Tested between the two windows and with a fake trader |
| Talking to friends | ✅ walk up to a friend and press A: their greeting (menu → Your greeting), then the trade question, open in the **game's own text box** (the game runs a tiny script of ours through its NPC-talk event; `Talk.cpp`), and they get "Name is talking to you!". Friends shown as markers only get a plugin-drawn box |
| Battling a friend's team | ✅ talk to a friend → "What do you want to do?" with the game's choice list: **Battle** / Trade / Cancel. Battle starts a real trainer battle (the game's own `_CallTrainerBattleCore`, as trainer scripts call it) against the rival that matches their character (May, trainer 906, or Brendan, 699: post-game single battles with the strongest AI), **named after the friend** ("You are challenged by Pokémon Trainer Bot!": the trainer data's name StrBuf, +0x28, is overwritten in the same hook), and a hook on the trainer loader (`FriendBattle.cpp`, `FUN_00367804`) swaps in **their party**, healed, nicknames and all. The game's AI plays it. It's friendly: started with `_CallTrainerBattleCore`'s "may lose" flag (4), so a loss doesn't white you out, and afterwards your party (HP, EXP, levels) and money are put back as they were, with a "You beat X's team!" / "You lost to X's team!" notice. Their party travels as PARTY frames (asked for when they join and whenever you talk to them). Tested against a fake friend with a copy of a real 3-Pokémon party in Granite Cave: May sent out their Marshtomp, Zigzagoon and Whismur; won; party back to Lv16 and money back to 7008. Lost on purpose too (all three fainted): back on the same spot, "You lost to Bot's team!" shown once the game's box closed, party back at full HP |
| Activity notices | ✅ friends get a notice when you catch a Pokémon ("Sami caught Aron!"), one evolves ("Sami's Marshtomp evolved into Swampert!") or you earn a badge ("Sami earned the Knuckle Badge!"). Read from the save data in memory (badges, PC boxes) and the party (`Activity.cpp`, `Save.cpp`), sent as EVENT frames. A catch = a Pokémon in the party or PC after a battle that wasn't there before it (party full → PC counts too). Tested in Granite Cave: notices from a fake friend, and our own badge/evolution/catch (faked in memory, then restored) reaching the fake friend; running from a battle sends nothing |

## Known bugs (found playing over Tailscale, 2026-10-19)

1. ~~**Talking to a friend crashes the game of the player who pressed A.**~~ **Fixed.** It was a hang, not a crash: the game's NPC-talk event ran script id 0 (our trainers have no script), which never resolves to a file, so the script runner looped forever. `Talk.cpp` hooks the run-script call (`FUN_003DC114`) and runs our message script for friends' trainers instead. Tested: 10+ talks in a row from both sides, no hang.
2. ~~**Every friend shows as the same trainer**~~ **Fixed.** Friends' models are only loaded with a map, and the first map after Continue loaded before any friend's model was known, so they showed as your own trainer until a map change. Now both Brendan and May are loaded on every map while Real models is on (one extra model), and a friend spawned with a stand-in is swapped to their own trainer as soon as it's available. Tested with a fake Brendan friend next to two May players, straight after Continue.
3. ~~**Losing a battle against a friend's team stopped the emulation**~~ **Fixed** (found 2026-10-20). The battle event whites the player out after a loss unless `_CallTrainerBattleCore` gets flag 4 ("may lose"); with flags 0 the whiteout ran under our talk script and Azahar went back to the game list as the battle ended. The talk script now passes flag 4. Tested: lost a full battle, back on the same spot, party restored.

## Notes for the next session

- Everything above is verified in Azahar with the fake friend (and a real running player for
  the run clip), and with two Azahar windows on one PC. Over the internet via Tailscale it
  connects and syncs (2026-10-19), but see Known bugs above.
- Details for every address/function are in `re/NOTES.md` (action table, anim byte mm+0x58,
  grid records, resident model table + list-loader hook, camera matrix, field-active flag).
- A battle does **not** reload the map's resident models (checked: a model wanted mid-map still
  wasn't loaded after a battle); only a real map change or Continue does.
- Two Azahar processes can end up running at once (an old window left open): both join the room
  with the same name, and the other one's trainer stands in your game as a solid character.
  If a ghost "you" shows up, check Task Manager for a second azahar.exe.

## How to pick it up

1. Build (WSL, because Smart App Control blocks devkitARM on Windows):
   `bash /mnt/e/oras-mmo/scripts/build.sh`
2. Install: copy `plugin/oras-mmo.3gx` to
   `%APPDATA%\Azahar\sdmc\luma\plugins\000400000011C500\`
3. Server: `node E:\oras-mmo\server\server.mjs` (port 7777). Fake friend for testing:
   `node E:\oras-mmo\tools\fakefriend.mjs --map <id> --x <x> --y <y> --radius 30 --speed 0.15 --stopgo`
   (`--anim 0x19` run clip, `--model 0xab` May, `--x2/--y2 --after N` jump to test collision,
   `--map2 <id> --after N` walk through a "door" to another map, `--nowarp` flag that as Fly,
   `--party <file>` a team to battle, `--listen` prints others' states incl. height z)
4. Memory tools (Azahar GDB stub, port 24689): `node E:\ORAS-RE\tools\mem.mjs`
   — `watch`, `read`, `dump`, `find-pos`, `find-u32`, `write-float/u32`, `catch-write` (`SET=` to
   write first), `catch-break`, `append-model`.
5. Ghidra project: `E:\ORAS-RE\ghidra\ORAS_AS_1_4.gpr` (code.bin + DllField block);
   headless scripts in `E:\ORAS-RE\ghidra_scripts` (Decomp, Listing, FindStores, ListFuncs…).

## Current machine state

- Azahar settings changed: plugin loader **on**, GDB stub **on** (backup: `qt-config.ini.bak-oras-mmo`).
  Turn the GDB stub off for normal play — it slows things when a debugger is attached.
- Plugin config `oras-mmo.txt` has `puppets=1` (Real models on).
- GDB stub port is now **24690** (`GDB_PORT=24690 node mem.mjs ...`).
- Azahar has a second input profile **Keyboard** (A=A, B=S, X=Z, Y=X, Start=M, Select=N,
  arrows = Circle Pad) for automated testing; **Default** (the controller) is the active one.
- Save file: never saved by automation. Tests on 2026-10-20 wrote memory only (a badge bit,
  party slot 0's species, box slot 1) and restored each one.
- Plugin menu (SELECT) also has two debug entries: spawn test character / action on test character.
- `oras-mmo-models.bin` next to the plugin holds the descriptor of model 0x13C from the outfit
  test (harmless: only used when a friend reports that model; delete it to forget it).

## Ideas queue

- **More battles.** A friend as a **tag partner** (multi battle, `FUN_001DBE74`, client 2) needs a
  partner trainer id and the same hook (name included). The trainer class still says "Pokémon
  Trainer" (the second StrBuf, +0x2C). Live player-vs-player (both people choosing moves) would
  need the link-battle layer.
- **More through talking.** Chat still needs SELECT → Send chat (typing needs the keyboard).
  The rest of the plugin menu is setup and debug entries.
- **Trade animation.** DllField has CallFieldTradeDemo (the in-game trade cutscene); playing it
  after a trade would need its arguments worked out, like the list menu's.
- Warps are learned per session; saving them (like the model cache) would label doors from the
  start. Off-screen doors get no arrow yet.
