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

### ✅ oras-coop "Real models" mode works (plugin v0.1.0, 2026-10-18)

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
