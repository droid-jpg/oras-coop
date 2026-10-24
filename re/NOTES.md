# ORAS MMO — reverse-engineering notes

Target: Pokémon Alpha Sapphire, title `000400000011C500`, update `0004000E0011C500` **v1.4**.
Internal app title: `sango-2`.

## Files

| Path | What |
|---|---|
| `as_v1.4\code.bin` | Decompressed 1.4 ExeFS `.code` (5,439,488 bytes) |
| `as_v1.0\code.bin` | Decompressed 1.0 `.code` (base cart) |
| `as_vX\layout.json` | Segment layout from the exheader |
| `ghidra\ORAS_AS_1_4.gpr` | Ghidra project (ARM:LE:32:v6, base 0x100000, 15,533 functions) |
| `tools\mem.mjs` | Read-only GDB-stub client for Azahar (port 24689) |

## Memory layout (1.4)

| Segment | Address | Size |
|---|---|---|
| .text | 0x00100000 | 0x479E04 |
| .rodata | 0x0057A000 | 0x71C24 |
| .data | 0x005EC000 | 0x439F4 |
| .bss | after .data | 0x7EC2C |

## Real player models — constraints

- Walking animations exist for all NPC models, but **running animations only exist for a few
  key characters** (Steven, Brendan, May, Zinnia, maybe Wally). Hijacking a generic NPC means
  friends who run will be shown walking. Final approach should spawn a Brendan/May model.

## Field character list (found 2026-10-18, Littleroot, map 6)

Every on-map character (player included) is a separate 0x310-byte heap allocation, placed
consecutively and doubly linked via the allocator header:

| Entry (header) | Object (+0x10) | Who | Position (first copy, entry+0x180) |
|---|---|---|---|
| 0x08299524 | 0x08299534 | NPC south | 1899 / 3249 (walking) |
| 0x08299844 | 0x08299854 | Kid NPC | 1791 / 2925 |
| 0x08299B64 | 0x08299B74 | NPC | 1863 / 3339 |
| 0x08299E84 | 0x08299E94 | **Player** | live |

Header (allocator, not game data): `u16 0x5544 'DU'`, `u32 size 0x310` @+4, `prev` @+8, `next` @+0xC.

Entry-relative fields (entry = header address):

| Off | Player | Kid | Guess |
|---|---|---|---|
| +0x10 | 0x62 | 0x62 | type/flags |
| +0x30 | 0x007FAC30 | 0x007FAC30 | vtable — **outside code.bin** (> 0x62FFFF) → class lives in a CRO module |
| +0x40..+0x4C | 3.0, 2.0, 14.0, 1.0 | 0, 0, 1.0, 1.0 | speeds/scales? |
| +0x50 | 0xDF | 0x94 | **model ID?** |
| +0x74 | 0x0004022B | 0x00020016 | object/script ID? |
| +0x7C | 0x00600120 | 0x00600120 | pointer into code.bin .data |
| +0x180 / +0x1B4 / +0x2D0 | x, z, y | x, z, y | position copies (render transforms nearby) |
| +0x284 | 0x005DDC24 | 0x005DDC24 | vtable of an embedded sub-object (code.bin .rodata) |

Manager-like object just before the array at ~0x08299478 (rodata ptrs 0x005DDC08, 0x005DE4AC);
0x082994E8 points to the last entry (player), 0x082994D0 to the first.

Player "logic" object 0x08C6E880 does not point directly into this list.

Next: extract CROs from RomFS to analyse class 0x007FAC30; test whether +0x50 is the model ID.

### Write test (2026-10-18) — moving the kid NPC (X 1791 → 1809)

1. Wrote the 3 character-list copies (entry+0x180/+0x1B4/+0x2D0): **reverted within 1 s**.
2. Wrote all 7 heap float copies of X (adds 0x0829990C = entry+0xC8, matrix-style with Y at +0x20,
   and 0x08D23534/5C/A0): **all held**, kid **did not move** on screen.
3. Wrote 2 linear-heap matrix translations (0x14D025CC, 0x14D02664; Y at +0x20): held, **no move**.

Conclusion: the rendered transform is downstream (likely skeleton bone matrices recomputed only when
the character is marked dirty/moved). Direct value poking is a dead end; next step is static
analysis of the field-character class (vtable 0x007FAC30, in a CRO) to find and call its
set-position / spawn functions. Tile spawn tables live at 0x08D06xxx and 0x08D1Dxxx
(u16 tile pairs + script IDs) — static map data, not live state.

## Move models — the authoritative character position (found 2026-10-18)

Tools: `mem.mjs catch-write` (GDB write watchpoint; Azahar also stops on reads, so trust only
hits marked WRITE; reported PC is the *next block*, look a few instructions back / at LR).

- **Move model (mm)**: per-character object holding the real position.
  - `mm+0xE8` position record: u16 kind (0) @+0, f32 X @+4, f32 Z(height) @+8, f32 Y @+0xC (0x2C bytes)
  - `mm+0x114` vec3 used by the step (velocity/target — for an idle NPC it equals its position)
  - `mm+0x4` flags (bit 0x4000000 checked in step), `mm+0x8A0` → map/zone object
  - Player mm = `*(controller+0x0C)`; controller (DllField) observed at 0x082C8030 → mm 0x08D249A8.
  - Kid NPC mm = 0x08D23448 (record @0x08D23530).
- **Step / integrator**: `FUN_003F1768(mm)` — builds temp pos from map + mm+0xE8, applies mm+0x114
  with collision (`0x19C560`, import stub), writes back to mm+0xE8 (`0x19C330`, import stub),
  then `FUN_003F413C(mm,&pos)` (slope/push: adds to mm+0x114). Writer PC observed 0x003F17C4.
- **Player state sync**: `FUN_0076BE9C(controller)` (DllField) copies mm+0xE8 record →
  player state object 0x08C6E880 (+0x10..+0x38) via `FUN_003C8B80`; facing from
  `FUN_004DBFB8(mm)` → +0x06; map id from `*(controller+0x34)+0xF0` → +0x04.
  So 0x08C6E880 is a **downstream copy** (fine for reading, useless for writing).
- **Position compare**: `FUN_003EAA9C(a,b)` compares two records within tolerance (s0/s1).
- Many `thunk_FUN_00146078` / `0x0019Cxxx` calls are **CRO import stubs** (jump via pointer
  filled at load time, e.g. DAT_0019C334) — resolve targets from live memory.
- **Rendering**: idle NPC render matrix (kid @0x14D025CC) is neither read nor written while
  idle — the model transform is only pushed while the character is moving. Player render
  transform near 0x14D0F4B8. → To move an NPC visibly, drive it through the movement path
  (activate it + set target/velocity), don't poke positions.

### ✅ Moving an NPC's real model (2026-10-18)

Kid NPC (mm 0x08D23448) teleported 3 tiles (X 1791 → 1845) and **visibly moved** on screen:
1. write f32 `mm+0xEC` (X) [and `mm+0xF4` Y, `mm+0xF0` Z as needed]
2. write the same to `mm+0x118`.. — i.e. the committed copy at `mm+0x114` (X), `+0x118` (Z), `+0x11C` (Y)
3. set bit `0x400` in u32 `mm+0x08` ("moving" — player shows 0x20004402 walking vs 0x20004002 idle)
The flag stayed set afterwards (not auto-cleared) — check side effects (animation state, collision).
Flags compare: kid `+0x04`=0x08010003 `+0x08`=0x00004002; player `+0x04`=0x00010443 `+0x08`=0x20004002.

### Move-model array & facing (2026-10-18)

- 32 slots, stride 0xAB0, vtable 0x005DF560 at +0; Littleroot base 0x08D22998.
  Slot in use ⇔ `+0x04` bit 0. `+0x0C` u16 object id (player 0xFF), `+0x10` u16 model id,
  `+0x16` u16 script id (matches map spawn table entries).
- Facing = unit vector (cos θ, sin θ), θ from east toward south: cos at +0x1C/+0x28/+0x34/+0x48,
  sin at +0x24/+0x30/+0x3C/+0x40 (diagonal: all 0.707). Player-state facing: 0 up, 1 down,
  2 left, 3 right, 7 down-right (4–6 other diagonals).
- Animation-ish fields that change with facing/state: +0x824, +0x830, +0x904, +0x90C.

### ✅ oras-mmo "Real models" mode works (plugin v0.1.0, 2026-10-18)

Plugin drives the nearest free NPC's move model to each remote player every frame
(pos, committed copy, facing, moving bit), restores it on release. Verified in Azahar with
fake bots: stationary East/South shown by kid + man NPCs; circling Bot shown by the kid.
Not yet verified: walk animation, facing correctness, label alignment (overlay scale guess).

### ✅ Spawning (injecting) new characters (2026-10-18)

- Manager (Littleroot/house): 0x08D1ED3C. `+0x369C` → move-model array, `+0x36A0` u16 slot count
  (32). Each move model stores the manager at `mm+0x8B4`; zone id at `mm+0x0E`.
- Map spawn loop: `FUN_003F8358(manager, entries, count, zone, extras[])` — entries are 0x30 bytes,
  skips entries hidden by event flag (`FUN_0012FE90`), then calls the spawn fn and sets
  flags4 |= 0x08000000 on map-loaded NPCs.
- **Spawn**: `FUN_003F584C(manager, entry*, zone, extra)` → returns mm (or asserts if no free slot).
  Reuses a matching persistent slot, else first free slot; then `FUN_003F97C8(mm, entry)`
  (copies fields, `FUN_003F8E7C` position, `FUN_003F663C` model) and `FUN_003FAA38`.
  Observed calls: map loader (lr 0x3F841C) and DllField player spawn (lr 0x794998), both extra=0.
- Entry (u16 words): [0] id → mm+0xC, [1] model → +0x10, [2]&0xFF move type → +0x12,
  [3] type → +0x14, [4] event flag → +0x16, [5] script → +0x18, [6]&0xFF dir → +0x1A,
  [7..9] → +0x4C.., [10..13] → +0x5A.., [0xE] → +0x52, [0xF] → +0x54, [0x11] → +0x56,
  [0x12..0x13] u32 (≠0 → alternate/rail position mode), [0x14] tile x, [0x15] tile y.
  Player entry built by DllField: `ff 00 ac 00 01 00 …; [0xE..0x10]=0xFFFF; tile (8,18)`.
- **Models** `FUN_003F663C(manager, model, mm)`: (1) manager's resident table (+0x36C8 count,
  +0x36CC entries of 0x18) → copy handle to mm+0x884..0x898; (2) else share from another live
  mm with the same model; (3) else load from archive via `FUN_00128E4C(manager+0x36B4, …)` —
  asserts if loading isn't allowed at that moment. → Use the player's model (always loaded).
- **Test**: plugin called the spawn fn from the CTRPF menu (id 0xF0, model 0xAC, move type 0,
  tile = player +2 x) inside the house (map 225) → new trainer visible, slot 2 (0x08D23EF8). No crash.
- **Despawn**: `FUN_003FB3A0(mm)` — releases render/anim objects (vtables at mm+0xA10/0xA14/0xA18),
  resets the mm, sets +0x08 |= 0x1800, slot becomes free. Called by map-exit cleanup
  `FUN_003F5D98(manager, force)` (skips persistent bit 0x40000 unless forced) and script commands.
- ✅ Plugin v0.1 "Real models" now spawns a trainer per remote (id 0xE0+n, player's model),
  drives it each frame (pos/facing/moving bit), despawns on leave. Verified in the house
  (map 225) with a circling bot: moves, turns, despawns cleanly (slot freed, no crash).
  Spawning waits 1 s after a map change; on map change the game deletes it itself.
- Still needed: other models (Brendan/May for the opposite gender) — needs the archive load path
  (tier 3 of FUN_003F663C) or a resident copy; walk/run animation while driven.

### Animation / actions (2026-10-19, in progress)

- Idle vs walking (player mm): +0x904 action code (0 idle, 0x13 while walking), +0x908 progress,
  +0x90C sub-state|counter<<16, +0x91C/920/924 step target position, +0x824 anim index/frame,
  +0x08 bit 0x200 while an action runs.
- **Set action**: `FUN_003FB1B4(mm, code, dir)` → DllField `FUN_0072D128(mm+0x8FC, code, dir)`:
  code ≤ 0x103 at mm+0x904, dir byte at mm+0x906 (9 = keep facing), resets progress (+0x908=0),
  sets +0x08 |= 0x200. Asserts if mm+0x9F8 && mm+0x9FC.
- Observed codes: player walking 0x13 → 0x01 → 0x65; wandering NPC calls code 1 every frame
  (likely "stand"); event FSM `FUN_003DC3DC` issues 7.
- Tests on a spawned trainer: 0x13 dir 0 → turned to face up, no step; 0x65 dir 0 → nothing;
  calling 1 every frame → progress stuck at 0, **legs don't move** (user confirmed).
- Next: `FUN_003FF4D0` issues actions from a movement-command queue (entry +0x0A code,
  +0x0C dir) — the cutscene "apply movement" path. Find that command table (walk/run per dir,
  step duration) and drive trainers by queuing step commands toward the remote position.
- Model animation API seen in DllField `FUN_0072D500`: model vtable +0xC0 (set anim slot),
  +0x100, +0xD4 — fallback for driving clips directly.

### ✅ Walk/run animation solved (2026-10-19)

- **Action table**: global array at `*(0x007301AC)` (live 0x081884D0), 0x104 pointers, one
  0x48-byte object per action code, built by DllField `FUN_0072F1E4`. Object: +0 vtable,
  +4.. phase functions, +0x44 phase count. Action progress (+0x908) = phase index.
- Step actions 0x0B–0x19: phase 1 picks frames-per-tile + anim, phase 2 `FUN_00774D9C`
  interpolates, phase 3 `FUN_0072D120` ends. Each calls **`FUN_003F1C20(mm, anim)`** which just
  sets **`mm+0x58` = animation id** (remapped through a table only if mm+0x59 < 0x17 and set).
- Anim ids (frames per tile): 0x10 walk (7, the player's normal walk, action 0x13), 0x19 run
  (verified visually on the player model), 0x0F (8), 0x0A (16), 0x05 (32), 0x11/0x17 (6),
  0x18 (5), 0x14 (3–5), 0x15 (2–3), 0x16 (1). 0 = idle. Action 1 ("stand", `FUN_0072CB34`)
  sets anim 0 every frame.
- The "legs don't move" bug was our own per-frame `SetAction(1)` (stand) resetting the anim.
  Spawned trainers (move type 0) get no actions from the game, so writing mm+0x58 directly
  sticks: 0x10 → legs walk, 0 → stand. Plugin now does this per frame; the sender forwards its
  own mm+0x58 in STATE @36, receiver falls back to speed (walk ~77 units/s, run > 105).
- Walk speed: 18 units / 7 frames at 30 fps.

### ✅ Character collision (2026-10-19)

- mm has three 0x2C grid records: `+0x64` previous, `+0x90` current, `+0xBC` next tile
  (f32 tile x @+4, tile z @+8, tile y @+0xC; tile = floor(world / 18)). Spawn fills all three
  from the entry tile (`FUN_003F8E7C`).
- The player is blocked by a character on the tile in these records (verified: trainer moved
  by position only kept blocking its spawn tile; after writing its current tile the player
  stopped right in front of it). Plugin writes all three to the current tile every frame.

### ✅ Other trainer models — May/Brendan (2026-10-19)

- Player model by gender (DllField `FUN_0070A614`): 0 → **0xAC Brendan**, 1 → **0xAB May**.
  Archive file index = model id for both. Loaded once into resident slot 0 by `FUN_003F7640`
  (flag manager+0x36AB) via `FUN_00128E4C(manager+0x36B4, FUN_0019C678(model), manager+0x36CC)`.
- `FUN_00128E4C(archive, file, out)` = generic synchronous file read (60+ callers); during a map
  load it's only used for descriptors (player slot 0, variable models), not 3D data.
- Resident table: u32 count @0x36C8 (copy @0x36C4), 0x18-byte descriptors @0x36CC
  (u16 model, 01 01 04 00 01 01 01 02 01 01, 0, 0, u32 file @+0x14), max 0x20.
  List loader **`FUN_003F7728(manager, listFile)`**: state byte +0x36AA (0 = start: memset
  table via FUN_003F6CEC, write both counts = size/24 + 1, async-read the list into slots 1..;
  1 = poll). Littleroot house: 11 models; outdoors: 30.
- **Key result**: models in this list get their 3D data loaded during the map load. A descriptor
  appended mid-map spawns an invisible character; one appended right after the loader writes the
  counts (state 0 → 1) loads normally — verified with a GDB write-watch (`mem.mjs append-model`)
  and then with the plugin's MITM hook on FUN_003F7728: May (0xAB) visible + animated, from the
  first load after boot and on every map change.
- Variable models 0x7A–0x89 (`FUN_003F7578`): descriptor replaced by save var
  (work+0x13140, index model+0x3FA6). **0x7A = rival**, in this save → file 0x13A = May
  (verified visually). Resident wherever the story shows the rival (Littleroot). Plugin uses it
  as a fallback for a May friend who joined mid-map.
- **Any model's descriptor (2026-10-20, verified)**: `FUN_0019C678(model)` → descriptor file (-1 =
  none; 0x00146078, which Ghidra shows as the "thunk", is the abort routine), then
  `FUN_00128E4C(*(manager+0x36B4), file, out)` reads the 0x18 bytes; same as romfs `a/0/2/2[model]`
  (checked for 0x13C). Called from the plugin on the plain field. Descriptor byte 2 = 1 for player
  models (0xAB, 0xAC, NPC copies 0x13A May / 0x13B Brendan). A model appended this way at map load
  renders and animates (0x13C, a cyclist). **Battles don't rerun the list loader**: only a map
  change or Continue loads newly wanted models.
- Dead ends: `FUN_00402FCC`/`FUN_00402D88` = area/terrain loader, not characters; manager+0x3C20
  (tier-3 "load allowed" flag) never set during map changes.

### ✅ Camera / screen projection (2026-10-19)

- Field camera object: word `0x00020003`, pointer, then a **4x4 row-major view-projection
  matrix** (world x, height, map y, 1 → clip), eye at matrix+0x4C, target at +0x58.
  Top screen: sx = 200·(1 + cx/cw), sy = 120·(1 − cy/cw). 30° vertical FOV.
  Address varies per boot (seen 0x08286210, 0x082848C0); a stale copy can remain → validate
  that the player's feet project on-screen. Littleroot: follow camera (feet at y 148);
  house: fixed camera (eye 153,125,488 → target 153,30,284; feet at y 227).

### Field state / intro hang (2026-10-19)

- During the intro movie the save's field is already built: player object valid, move models
  present, player mm+0x08 = 0x00000005. Calling the spawn function then **hangs the game**
  (friend connected before Continue). In the running field mm+0x08 always has 0x4000
  (0x4002/0x4006/0x4206/0x440F) → plugin gates everything on it.
- Real running (B held): player anim **0x19**, 9 units per 67 ms (~134 units/s; walk ~77).
  Indoors/without B: 0x10.

### Bikes, soaring, menus (2026-10-19, completed save "Nina", May, 8 badges)

- Player mm while riding the **Mach bike**: mm+0x04 0x00010443 → **0x03210443** (bits 0x01200000;
  0x02000000 alone is set indoors, e.g. 0x02010443 in a Pokémon Center), mm+0x59 form = 2,
  mm+0x58 anim 0x38 while moving (0 standing), mm+0xA2C 1 → 2, mm+0x974 0x101 → 0x3801.
  Speed ≈ 252 units/s (42 tiles in 3 s). Acro bike not in this save (ride speed = running).
- The bike is a separate object managed by the player controller (DllField ~0x79AB..);
  copying form/anim/flags onto an NPC trainer does not show a bike. Not reproduced yet.
- mm+0x08 bit 0 = under script control: 0x2000400F while **soaring** (Eon Flute), 0x5 in the
  intro; the start menu leaves it clear (0x20004002). Map id/position stay at the takeoff spot.
- Player state +0x08 ("aux") flips between 0x0/0x1/0xC with movement — not a bike flag.
- **Field-live check corrected**: player mm+0x08 bit **0x2** is set in every field state
  (towns 0x4002/0x4006, routes **0x8006**, bike 0x20004402, soaring 0x2000400F); the intro has
  0x5. 0x4000 (first guess) is clear on routes — that broke all syncing outside towns.
- **Wild battle** (Route 110 grass): bit 0 (script control) is set, so friends see "busy" after
  2 s; our own overlay is hidden during it. Fleeing clears it.
- **Surfing** (Route 109): player mm+0x04 0x00010443 → **0x13210443** (bit 0x10000000 + the bike
  bits), form mm+0x59 = 7, anim 0x54, mm+0x8A4 changes (attached sea mount?). Plugin: friends who
  surf get a water marker + "(surfing)", no trainer standing on the sea; back on land = trainer.
- **Bike mount** = DllField `FUN_007980BC` (controller r4 0x082C66E0; +8 manager, +0xC mm):
  `FUN_003F5C2C(manager, mm, 0x1B, 0)` until non-zero → `FUN_003F31EC(mm, 2)` (form) →
  `FUN_003F1C20(mm, 0)` → `SetAction(mm, 0x51, facing)` → `FUN_00783648(manager+0x36C0, 0xB, mm)`
  (effect) → flags 0x200000, 0x1000000, render-dependent 0x2000000 + render+0x140 |= 0x400 →
  `FUN_003F9C08(mm, 2)` (mm+0xA2C ride mode). Dismount step `FUN_00795A60`: anim 0, ride mode 1.
- `FUN_003F5C2C(manager, mm, ride, slot)`: ride-model pool at manager+0x36BC (status 3 = loaded →
  attach to the model instance `FUN_004E3F90(mm)`); returns 1 also on non-attach paths.
- **✅ Solved (2026-10-19)**: stubs resolved live (`ldr pc,[pc,#-4]` + target):
  0x19C608 → DllField FUN_00768FE0 (slot used? inst+0x1C0+slot, 2 slots),
  0x19C610 → 0x7D2DB8 (ride status in pool; 3 = loaded), 0x19C618 → 0x735A94 (request),
  0x19C620 → **FUN_007352AC(pool, inst, ride, mm, slot)** (creates the ride object, stores it in
  inst+0x1C4+slot*4), 0x19C628 → 0x72CDE4(mm+0x96C) (refresh). inst = FUN_004E3F90(mm) needs
  mm+0x08 bit 2 and mm+0xA18 — **0 for a few frames after spawning** (that was the earlier failure).
- Plugin `AttachRide` does these steps itself, then FUN_003F31EC(mm, form) + FUN_003F9C08(mm,
  mode), and plays the rider's own anim byte (bike seated 0x38). Verified: a friend's trainer sits
  on the **Mach bike** and rides/turns; dismount → respawn plain.
- **Surf mount** = DllField **FUN_007958B8**: FUN_003F5C2C(manager, mm, **ride 1**, 0), form **7**,
  anim 0, action 0x66, render+0x140 |= 0x400, FUN_0077BCF8/BCCC, effect 8, flags 0x200000/
  0x2000000/0x1000000/0x10000000, ride mode **0**, FUN_0077AEE0/AF80(inst, -1). With ride 1 /
  form 7 / mode 0 a friend's trainer sits on the surf Pokémon (verified); landing → respawn.
  Found via a breakpoint on the flag setter FUN_003F1688 (r1 = bit) — read-watchpoints on the
  flags word fire every frame and stall the game.
- Registered items wheel (Y): left Mach bike, right Eon Flute, up/down rods; pick with the D-pad.
- GDB caution: a `mem.mjs` call killed by `timeout` can leave the emulator halted → the
  game freezes and the relay drops the player; `scratchpad/resume.mjs` sends c + D.

DllField.cro: loaded at 0x006F3000 (header magic FIXD), code 0x006F3180+0x10EE80, name @0x00802000.
Added to the Ghidra project as block "DllField" from a live dump (relocations already applied).

## Player object (confirmed live on 1.4, 2026-10-18)

Object base observed at `0x08C6E880` (heap). Same addresses SamuTecYT/ORAS-MMO hardcodes.

| Offset | Address | Type | Meaning |
|---|---|---|---|
| +0x00 | 0x08C6E880 | u32 | vtable → `0x005DEDC0` (in .rodata) |
| +0x04 | 0x08C6E884 | u16 | map ID (6 at test location) |
| +0x06 | 0x08C6E886 | u16 | facing (3 = right/east; 6 seen earlier) |
| +0x08 | 0x08C6E888 | u32 | 0x0C while walking, 0 idle after D-pad steps — but also seen 0x0C while idle after reboot; meaning unclear (anim/state?) |
| +0x10 | 0x08C6E890 | u32 | 0x08240000 alongside +0x08 = 0x0C, else 0 — unknown |
| +0x14 | 0x08C6E894 | f32 | X (east = +X) |
| +0x18 | 0x08C6E898 | f32 | height (2.0 on flat ground) |
| +0x1C | 0x08C6E89C | f32 | Y |

### Movement

- D-pad steps snap to tile centres: tile = 18 units, centre = 18·n + 9.
  Walk test: X 1881 → 1935 for exactly 3 steps right (+54 = 3 × 18).
- **Circle-pad movement is free (not tile-locked)** — positions are arbitrary floats
  (e.g. X 1831.08). Sync raw floats + facing and interpolate on the receiving side;
  use tiles only for map/radar logic.

### Finding the object robustly

- Static scan of .data/.bss (0x5EC000–0x6B0000) for pointers near the object found one hit:
  `0x0062F7D4 → 0x08C6E1BC` (object − 0x6C4). That block starts with heap pointers, not a
  vtable, and the surrounding static table is all heap pointers — likely allocator
  bookkeeping, **not** a game-owned player pointer.
- **Reboot test (2026-10-18): stable.** After a full restart the object was still at
  `0x08C6E880` (map 6, new coords X 1933.49 / Y 3298.59, facing 1). A vtable scan of
  0x08000000–0x09000000 found exactly one hit — the player object.
  Plugin strategy: try `0x08C6E880` (check vtable), else scan heap for `0x005DEDC0`.
- **Map-change test (2026-10-18): stable.** Entering a building: map 6 → 225, coords became
  map-local (X 153 / Y 333 = tiles 8, 18), facing 0 (likely north). Object still at
  `0x08C6E880`, vtable scan still a single hit. Coordinates are per-map → only show remote
  players whose map ID matches.
- Facing seen so far: 0 (likely north, after entering a door), 1, 3 (east), 6. Full mapping TBD.
- Still to test: flying, surfing, bikes, battles, PC menu (what the object looks like then).
- Plan: (1) ~~check whether the object address is stable across reboots~~ done;
  (2) runtime heap scan for vtable `0x005DEDC0` + sanity checks (map ID, finite coords);
  (3) use Ghidra xrefs to `0x005DEDC0` to find the constructor/class and a real owner pointer.

### Text, place names, UI flags (2026-10-19)

- **GetString** `FUN_00139A34(msgData, line, strbuf)`; decrypt `FUN_0013F758`: key starts at
  (line + 3) * 0x2983, rotl 3 per UTF-16 char. msgData: vtable 0x005DE400, +0xC in-memory flag,
  +0x18 header (u16 languages, u16 lines, ..., +8 = 0 → encrypted), +0x1C section (entry i at
  +i*8+4: u32 offset, u16 length). StrBuf: +4 buffer, u16 capacity +8, u16 length +0xA, +0xC valid.
- `FUN_00451440(ctx, strbuf, file, line)` = GetText with a small cache table at 0x00630CB0.
- **Zone table**: `*(0x005F45BC)` → 0x218 × 0x38 entries (resident; `FUN_003D99FC` resident mode).
  +0x1C & 0x3FF = place-name line (Littleroot 170, Slateport 0xBA, Route 109 220); `FUN_004D88E8`.
  Area banner: DllField `FUN_007126B0`, place text = resident 356-line file (`*(obj+0x48)+0x1C8`).
- **Field message window**: DllField `FUN_00747308(obj, params)` builds it (params +4 = line, -1 none);
  the Surf prompt fetches line 0xC there. Not driven by the plugin yet.
- **UI flags (player mm+0x08)**: idle 0x00044006, text box 0x0004400E (bit 0x8), X menu 0x0204400E
  (+0x02000000). Pressing A at a spawned friend does nothing in-game (no script).
- Two Azahar instances both bind GDB 24689 (SO_REUSEADDR) and connections alternate between them.
  Give the second its own port: `gdbstub_port\default=false` + `gdbstub_port=24690` before launch
  (with `default=true` the value is ignored).

### Field scripts and the game's own text box (2026-10-19)

- Field dialogue runs as **AMX (Pawn) scripts**: header magic 0xF1E0, file/VM version 10, flags 0x1C
  (0x4 = compact encoding, expanded by amx_Init `FUN_00505DB4`), defsize 8, natives imported by
  **name hash** `h = c ^ (h * 0x83)` (lookup `FUN_00505B04`). Interpreter `FUN_00503508`, jump
  table `*(0x0050468C)` by `op & 0xFFFF`; verifier `FUN_001D3630`. Opcodes used: 0x27 PUSH.C imm,
  0x2E PROC, 0x30 RETN, 0x33 JUMP rel, 0x35 JZER rel (relative to the opcode's address), 0x59/0x5A
  ZERO.pri/alt, 0x78 HALT imm (12 = sleep: resume next frame), 0x87 SYSREQ.N idx bytes,
  0xBC/0xBE packed PUSH.C/PUSH.S (operand in the high 16 bits), 0xD2 packed HALT.
- Native tables: DllField `0x7EA58C` = {name*, func} pairs (332). Message natives:
  TalkMdlMsg_Seq `0x747308` (19 args; the Surf check passes line, 0, -1, 0xB, 0, 0xAA, 0×10, -1, 0, 0),
  CheckWinAllSuspend `0x74B4FC`, LastKeyWait `0x744EF4`, MsgWinCloseNo `0x746458`, _Suspend
  (code.bin) = wait N frames, followed in scripts by ZERO.pri, ZERO.alt, HALT 12.
- **Run a script**: `FUN_003DC114(field, scriptId, target, p4..p7)` → event + request
  (`FUN_003FE85C` → `FUN_003FEB40` → `FUN_003FE7C0`). `FUN_003FB594(req+0x24, zone, id, x)`
  resolves the id: global ranges table `0x587694` (0x56 × 16 bytes: u16 min, max, file, heap, msg
  file) → common script file (e.g. 10050..10089 → file 0x2E Surf/field checks, msg file 254, native
  mask 0x643); other ids > 0 → the zone's script. g_mode = id; main() switches on it.
- Script manager `*(0x005F46FC)` = `*(field+0x54)`, field = `*(0x0062F7C4)`. +0x24 file container
  (u16 count, entries {code, size, u16 id; 0xFFFF free}), +0x28 pool (3 script objects, stack),
  +0x34 current script (0 = none). The loader `FUN_003AB0C8` copies the file (stp bytes) and runs
  amx_Init; the container buffer is then freed by `FUN_003FDD44` (game-heap free via header at -0x20).
- **Hang**: pressing A at a spawned friend runs its NPC script id **0**: unresolved request,
  file 0xFFFF, "loaded" without an AMX → `FUN_003AB254` loops on amx_Exec error 0x16 forever.
- Plugin (`Talk.cpp`): builds a 5-native AMX image, puts it in a free container slot as file 0x3F0,
  hooks FUN_003DC114 (id 0 + target = friend trainer → our id 32700) and FUN_003FB594 (our id →
  the 10050 request, file 0x3F0). Text: DllField's GetString import stub `0x006F32B8` is pointed at
  our function, which fills line 0x7FF0. (A CTRPF MITM hook on GetString itself crashed: worker
  threads call it and get no hook context.)

### Battles, the CRO slot, species names, the party (2026-10-19)

- **One CRO slot at 0x006F3000** holds DllField on the field and **DllBattle** in a battle; header
  +0x84 = name address (0x00802000 "DllField", 0x007DA000 "DllBattle"). In a battle the player's
  move models stay in memory with the field flags still set, but every DllField function and stub
  is gone: calling spawn/delete/actions/rides then would crash. The plugin gates `inField` and
  those calls on DllField being in the slot.
- **Battle state**: DllBattle code has the global's address in a literal at `0x0074580C`
  (`ldr r7,[pc,#0x68]` in the function at 0x745790). Global (heap) = { battle object, side table }.
  Side table +4: per side 6 pointers + u32 count, 0x1C bytes per side (side 0 = us, 1 = opponent);
  slot 0 = the Pokemon out (the order follows switches; +0x84 has the original order). Battle
  Pokemon: +0x0C species, +0x0E max HP, +0x10 HP, +0x18 u8 level. Verified on a wild Wingull (Route
  109 surf): 48/48 → 0/48 after Flamethrower; ours 219/300, 356/356, 120/120.
- **Species names**: resident message file with 812 lines (line = dex number), same address on the
  field and in battles (0x08021394 this boot); decoded like the place names. Moves: 622 lines,
  abilities: 192, items: 776.
- **Party**: 6 pointers at **0x08CFB1E0** + u32 count at 0x08CFB1F8 (boot-time allocation, same in
  every run seen). Slot object (vtable 0x005DE89C): +4 → encrypted PK6 data (0xE8), +8 → encrypted
  party stats (0x1C; level at +4, HP at +8/+0xA after decryption with the data's EC). Writing both
  blocks from another game's Pokemon shows up in the game's party screen straight away (tested).

### Script dialogs: Yes/No, choice lists, party screen (2026-10-19)

- DllField's native table (0x7EA58C, 332 {name*, func}) has the names; disassembler for game
  scripts: `ORAS-RE/tools/amx/disasm.mjs <script.bin> <natives.txt>` (packed ops P188 = PUSH.C imm,
  P190 = PUSH.S, P164 = LOAD.S.pri, P177 = STOR.S.pri; operand in the high 16 bits).
- **Pages**: the game chains messages in one open window: TalkMdlMsg_Seq, CheckWinAllSuspend loop,
  then SetKeyWaitMark / _ABKeyWait / VanishKeyWaitMark. Closing (MsgWinCloseNo) and immediately
  opening another message ends the script. A long line pages by itself (red arrow).
- **YesNoWin_Seq(0,0,0,0)** returns 0 until answered; the answer is in work 0x8010, read with
  native hash 0x8E50F765 (GetWork; its name isn't in DllField's table). Yes = non-zero.
- **ListMenuInit_Seq(x, y, ?, cancel, ?, ?)** (poll until non-zero), **ListMenuAdd(line, value, 0)**
  per item (the text comes through GetString for `line`), **ListMenuStart_Seq()** (poll); choice in
  work 0x8010, B = 0xFFFF (only with the 4th init argument = 1). y > 0 moves the list up; 20 px per
  item keeps it clear of the text box (tuned live by poking the arguments through GDB).
- **CallPokeSelect(mode 0..5, result work, filter, flag)** (0x746BDC): mode 0 opens the normal
  party menu (Summary / Use a move...), and returning from it straight into our script left the
  field faded to black (soft lock; real scripts fade around it). Not used.
- The party screen and bag run as their own CRO in the shared slot (name 0x00706000 seen), so
  "DllField loaded" doubles as "on the plain field".

### Save data in memory (2026-10-20)

- The save is one boot-time block in RAM, but packed differently from the save file. Found by
  matching the save file's blocks against a RAM dump (save file offset -> RAM):
  0x4200 -> **0x08C71DB8** (money u32 @+8, **badges u8 @+0xC**, one bit per gym),
  0x14000 -> 0x08C81340 (MyStatus: TID @0, OT name @+0x48), 0x15000 -> 0x08C81FE4 (Pokedex),
  0x1F400 -> 0x08C8B270, 0x31800 -> 0x08C9CA78, **0x33000 (PC boxes) -> 0x08C9E134** (31x30 x 0xE8,
  encrypted PK6, EC first; the usual NTR/PKHeX address). Delta isn't constant (0x8C6B134..0x8C6E9A4).
- The save on this machine (Sami, 1 badge, 7008 money, boxes empty) matched at the intro already:
  the save is loaded before Continue.
- Used by the plugin's activity notices (Activity.cpp / Save.cpp).

### Trainer battles and the battle setup (2026-10-20, static only)

- Script native **`_CallTrainerBattleCore`** (DllField 0x74E588), 7 args: u16 a1, u16 a2, u16 a3,
  u32 flags, u8 a5, u8 a6, u8 a7. Packed as { u8 type, u16 a1 @2, u16 a2 @4, u16 a3 @6, u32 flags @8,
  u8 @0xC..0xE }. a1 == 0: trainer data for a2 is read (`FUN_00454004`, GARC 0x24) and its byte +6
  becomes `type`; a1 != 0: type 1. Then `FUN_003DA09C` (thunk 0x6F6098) makes the battle event
  (0x290 bytes) and **`FUN_003DA118`** fills it:
  - setup (BATTLE_SETUP_PARAM) at **event+0x30**; by type: 0 + a3 != 0 → `FUN_001D8B94` (two
    trainers a2, a3), 1 + a1 != 0 → **`FUN_001DBE74` (multi: partner a1, foes a2, a3)**, else
    **`FUN_001D6E0C`** (one trainer a2).
  - flags: 0x10/0x20/0x40/0x80 → value 5/10/20/-10 to `FUN_003EAED0` (unknown), 4 → setup+0xA8 |= 0x100,
    1 → a filtered copy of the player's party goes in with `FUN_003B66C8(setup, party, 0)`,
    2 → `FUN_001E0518(setup)`, 8 → setup+0xAC = const.
- Setup: **party per client at +0x4C + client*4** (0 you, 1 foe, 2 partner, 3 second foe), trainer
  data per client at +0x6C + client*4 (0x38 bytes, `FUN_001F0558`; two string objects of 0xF and
  0x17 chars at [10]/[11], probably name and class), +0x1C s8 level offset applied to trainer parties.
- **`FUN_00367804(setup, gamedata, client, party, trainerId, heap)`** loads a trainer client: trainer
  data (`FUN_0045564C`), party (`FUN_004550A0`), then the level offset. Called for every trainer
  client (single, double, multi), so it is the hook point for putting a friend's team in a battle
  as the opponent (client 1) or as a tag partner (client 2).
- PokeParty = 6 pointers to Pokemon objects (all 6 allocated) + u8 count @0x18 + u8 lead @0x19
  (first non-fainted, non-egg); same layout as the save party at 0x08CFB1E0. `FUN_003B6754(party, pk)`
  appends, `FUN_003B63DC(party, i)` gets, `FUN_003B26D0(dst, src)` copies a Pokemon,
  `FUN_003B66C8(setup, party, client)` copies a whole party into a client's slot.
- Still unknown: how game scripts sequence a trainer battle (which natives wait for it, read the
  result, and whether the script or the event handles a loss / whiteout); capture a trainer's
  script from RAM on a route to see it. Decompiles in decomp/btl (ghidra_scripts/DecompRet.java
  clears "no return" on DllField's import thunks, which otherwise cut decompiles short).

### Trainer scripts, trainer data, game text (2026-10-20)

- **RomFS without extracting**: `tools/romfs/romfs.mjs <rom.cci> list|get` (decrypted .cci), GARC
  reader `tools/romfs/garc.mjs`, Gen 6 text decoder `tools/romfs/text.mjs`. GARCs copied to `romfs/`.
- **Common scripts = `a/0/2/9`** (85 AMX files; file index = the "file" column of the ranges table
  at 0x587694). Trainer battles: ids 3000..6999 → **file 81**. Its battle wrapper calls
  `_CallTrainerBattleCore(0, trainerId, 0, 0, 0, 0, 0)` and goes straight on to
  `_BattleGetResult(&buf)`: the script resumes once the battle is over. Result word 0 = lost → the
  script calls **`CallTrainerLose`**.
- **Correction (2026-10-20, seen in game): the battle event whites out by itself.** Battle event
  vtable 0x005DEF8C, update **`FUN_003DB1F0`**, state 5 after the battle: `FUN_003DABD8` = lost, and
  unless event+0x2B is set it calls `FUN_0044C768` (whiteout) and ends. event+0x2B is set only by
  `_CallTrainerBattleCore` **flags bit 4** (FUN_003DA118, which also sets setup+0xA8 |= 0x100): a
  battle you may lose. Losing a friend battle with flags 0 stopped the emulation as the battle
  ended (the whiteout ran under our talk script). The talk script now passes flags 4.
- Request: `+0x16` file, `+0x10` native mask (`*(0x587BF4 + file*4)`: 0x643 for both file 46 and
  81, so our Surf-template request can call the battle natives too), `+0x18` per-file value.
- **Trainer data `a/0/3/6`** (950 × 24 bytes): u16 @0 flags, u16 @2 class, u8 @6 battle type
  (0 single, 1 double...), u8 @7 Pokemon count, u8 @0x10 AI flags, u8 @0x15 money. Pokemon `a/0/3/8`.
  English text `a/0/7/3`: file 22 = trainer names, 21 = class names. Rivals: Brendan 1-3, 289-294,
  527-529, 674-676, 699-701 (post-game, AI 7); May 4-6, 295-300, 530-532, 677-679, 906-908.
- **Verified in game**: our talk script calling `_CallTrainerBattleCore(0, 906, ...)` starts "You
  are challenged by Pokemon Trainer May!", and a MITM hook on `FUN_00367804` that overwrites
  client 1's party objects (+4 data, +8 stats, count @0x18, lead @0x19) after the original puts
  the friend's team in: ball count, species, nicknames, levels all theirs. The battle mirror
  (`Battle.cpp`'s side table) is display-only: writing HP there doesn't change the battle.
- The talk script with a menu is ~650 AMX words: Talk.cpp's buffer was 480, and a failed build
  fell back to script id 0 (the old hang). Buffer raised; a failed build now shows "..." instead.

## GPU: how a frame reaches the screen (2026-10-20)

Why: CTRPF's OSD writes the overlay into the screen with the CPU. Azahar keeps an upscaled copy of
the frame on the host GPU; a CPU write into that memory makes it fall back to the guest-memory
copy (native 400x240), so every frame with the overlay on it lost the resolution scaling (menus,
where the overlay hides, stayed sharp). Fix: draw the overlay with the 3DS GPU inside the frame.

- **GSP submit**: `FUN_0017A598` = GSPGPU TriggerCmdReqQueue (IPC 0x000C0000); `FUN_0017404C(queue,
  cmd[8])` pushes a GX command into the shared-memory queue. Request builders: `FUN_00164810`
  type 0 (DMA), `FUN_00164890` 2 (memory fill), `FUN_00164900` 5+1 (flush + run command list),
  `FUN_00164CA8` 3 (display transfer), `FUN_001649D0` 4 (texture copy).
- **nngx**: global at **0x0066E008** (+0x9C current command list, +0xA0 the one running). Request
  runner `FUN_0015D930` walks the list's 0x1C-byte requests (cmdlist +0x18 array, +0x20 count,
  +0x24 next). Command list +4 buffer base, +0xC used bytes, +0x14 last split offset.
- **Command buffer write pointer: global 0x0061E0BC**, end 0x0061E0C0 (raw PICA commands, 8-byte
  aligned). **`FUN_0014298C` = nngxSplitDrawCmdlist**: appends 0x111=1, 0x110=1, 0x10=0x12345678 and
  records a type-1 request for everything since the last split.
- Transfer request (nngxTransferRenderImage): `FUN_00142500` -> `FUN_0015D5B8(cmdlist, src, srcW,
  srcH, srcFmt, dst, dstW, dstH, dstFmt, aa, ?, block)`.
- **Per screen: `FUN_00129F4C(display, renderTarget)`** (called from `FUN_0011CF38` after a split and
  an optional capture `FUN_00129A4C`) binds the display, queues the transfer, then splits. Display
  descriptors (0x2C bytes, two per screen, double-buffered): +8 screen (0 top, 1 bottom, 2 top
  right eye), +0xC/+0x10 400(320)x240, +0x14 AA mode (0 = none here), +0x1C 0x8051 (RGB8).
  Seen: 0x08C43020/4C top, 0x08C43078/A4 bottom; render targets 0x08C43198 (top), 0x08C431D8.
- Render target size: `FUN_0016B620` (width) / `FUN_0016B5D0` (height); colour buffer = +0x14 of the
  descriptor returned by virtual +0x10 of the buffer object at rt+0x1C (the two screens' buffer
  objects are different classes with different field offsets). Both screens render into the same
  **256x512 RGBA8 buffer at VRAM 0x1F070800** (depth 0x1F419800), copied to 0x1F300000 (top) /
  0x1F000000 (bottom) with srcW=256, srcH=512, starting 112 rows in.
- Mapping (test pattern): a screen of width W shows buffer x 0..240, y 0..W with screen x = W -
  buffer y, screen y = 240 - buffer x. Textures are read bottom-up (t = 0 is the last row).
- nngx allocator callbacks at 0x00616D84: alloc `0x0012A1B0(area, aim, id, size)` (0x10000 FCRAM;
  aim 0x101 texture = 128-aligned), free 0x0012A1D8. **Scene memory**: a texture allocated during
  the intro was reused by the title screen. The plugin takes its own linear memory instead
  (svcControlMemory MEMOP_ALLOC_LINEAR, 4 x 256 KB, works at 0x16Bxxxxx).
- Injecting a draw (shader upload, framebuffer, viewport, blend, texenv, immediate-mode quad)
  between the last split and the transfer caused no visible corruption of the game's following
  frames (title, intro cutscene, field, battles, X menu, map changes): the game sets its state
  again each frame.

### Trade evolutions (2026-10-24)

- **Link trade evolution** (`FUN_003E1D48` state 4, the Colosseum/trade proc): `FUN_004D2C68(received,
  given, &u16 species, &u32 route)` -> bool. Wraps `FUN_004D0C40(*0x617A64, ...)`: walks the species'
  evolution entries; method 5 trade, 6 trade holding item (param = item), 7 trade with partner
  (Karrablast 588 <-> Shelmet 616, needs the partner object; others accept partner = 0). Everstone
  check via vtable +8 of the evolution manager on the held item (skipped for Kadabra, 0x40); eggs never.
  `route` = entry index.
- **DllShinkaDemo param** (0x20): +0 PokeParty*, +4 index, +8 species, +0xC route, +0x10 u8 flags
  (trade: 0,0,0,1; after battle `FUN_003E8C68`: 1,1,1,? and +0x14 = MyStatus gamedata+0x129A0),
  rest 0. Battle level-ups pass the player's own party + slot, so the scene evolves in place.
  Launchers: `FUN_00369658(procMgr, param)` (Sequence + BackGround; no code.bin callers) and
  `FUN_003697B0` (also loads DllBattle; link trade and post-battle).
- **App calls from the field**: the request lives on the script manager (`*0x005F46FC`, same object
  as Talk.cpp's): `FUN_003D05E4(mgr, type, paramSize, doneCallback, x)` sets +0x1C type, +0x20 param,
  +0x24 pending, +0x2C callback. Natives fill it (CallFieldTradeDemo type 0x15 via DllField thunk
  0x6F5BF0; CallApplicationDemo 0x74BE5C type 0x16, 0x24-byte param, callback 0x78A1CC frees
  param[0]/[2]). The script manager's state machine `FUN_003D06B4` starts it in state 1 with
  `FUN_003CFBA8(mgr, gameMgr)` (switch on type, procMgr = gameMgr+0x14; types 1 and 0x12 do nothing).
  No type launches DllShinkaDemo, hence Evolve.cpp's hook on `FUN_003CFBA8`.
- Pokemon object (0x10, vtable 0x5DE89C): +4 data, +8 party stats, +0xC accessor; vtable +8 =
  deleting destructor. Blank one: `FUN_0011F560(mem, heap, 0, 1, 0, 0)`.

### Live battles: the battle server, adapters, RNG (2026-10-24)

DllBattle decompiled whole into `ORAS-RE/decomp/all_btl/<addr>.c` (ghidra_scripts/DecompAll.java,
program code_btl.bin, range 0x6F3000-0x7DA000). Same design as Gen 5's btl_server/btl_client.

- **Main module** (`param_1` of the setup procs): +0x10 setup, +0x18 TinyMT state = the battle RNG
  (calc globals `0x08152AD8` +0x54 points at it), +0x28 its 16-byte seed copy, +0x815 my client id,
  +0x818 battle style (config bit 3), +0x820 server, +0x828 + id*4 clients. Init `FUN_00707740`:
  setup+0x4B == 1 (replay) takes the seed from setup+0x128, else `FUN_001122E4` seeds from the clock.
- **Setup procs**: PMF table at 0x7CE2B8. One-console single battle = `FUN_0070956C`: state 6 server
  `FUN_0076677C` (copies main+0x28 into the calc RNG again), 7/8 clients `FUN_007662E0(…, id, type)`
  (type +0x91: 0 UI, 1 AI, 2 replay; AI object made only for type 1), 0xB `FUN_00766110` attaches
  each client's adapter (client+0x54) to the server (+0x1C + id*0xC: adapter, party, count, id @+0x27).
- **Adapter** (`FUN_007684B8`): +0 answer*, +4 answer bytes, +8 u16 command, +0xC command data,
  +0x1781 client, +0x1782 state (1 sent, 3 waiting, 4 answered), +0x1783 low nibble answered /
  high nibble link battle. Server side: `FUN_007682C8(server, cmd, data, size)` sends to all,
  **`FUN_00768150(server)`** polls all (link: `FUN_0042CDE4`/`FUN_0042CE08` net), `FUN_007A0AEC`
  = "answered?", `FUN_00768018` copies answers (u32 each) to server+0xB4 + client*0x18,
  `FUN_00767DA8` builds the replay record (cmd 10). Client side: dispatch `FUN_006F7984`, table
  0x7CEC68 {u8 cmd, u8, PMF UI, AI, replay}; answers via `FUN_0076DBBC(adapter, ptr, size)`.
- **Commands**: 3 pick actions (server `FUN_007044C0`), 5 send-in after a faint (`FUN_0070508C`),
  6 change (`FUN_007055CC`), 4 Shift-style "switch?" (only to my client, `FUN_007017F0`; answer 0 =
  No), 7 run? (wild), 8 play server commands.
- **Action word**: low nibble kind (1 move, 2 item, 3 switch, 4 run, 6/8 rotation/mega?), move:
  target position bits 4..7 (0xA = the game's default in singles), move id bits 8..23, bit 27/28
  flags; switch: `3 | pos << 4 | slot << 7` (AI: `FUN_0070138C`).
- **RNG**: TinyMT32, `FUN_0010FC58(state, seed)` init, `FUN_00175CEC(state, n)` → [0, n); calc
  wrapper `FUN_007436F8(n)`. The AI saves/restores the battle RNG around its simulations (dbee4/dbed0).
  Client RNGs (+0x58, +0x68) are separate.
- **Order**: `FUN_0075FA58` lists the actions client by client, then `FUN_0075E010` sorts by priority
  word with `rand(2)` on ties (bubble, list order matters); special priority (Quick Claw) rolls after
  the first sort. `FUN_0075E1B8(list, flow)` sorts Pokemon by speed (`FUN_00760414`) the same way.
  Pokemon id → client: `FUN_0076D770` (0..5 → 0, 12..17 → 1, 6..11 → 2, 18..23 → 3).
- **Player-only extras** in a one-console battle: EXP (`FUN_0075F034`, off with setup+0xA8 0x200 or
  0x2000), Amie affection into battle Pokemon +0x154 (`FUN_00766A64` → `FUN_004D2D84` = PK6 0xA3/0xCB,
  only for the client with player data at main+0x838), Shift question, O-Powers (not handled).
- DllBattle import stubs: 0x6F36A8 → `FUN_004EA27C` (battle style), 0x6F4A18 → `FUN_004D2D84`.
