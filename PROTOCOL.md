# oras-coop wire protocol (v1)

TCP, little-endian, **fixed 48-byte frames**. Every frame starts with:

| Off | Type | Field |
|---|---|---|
| 0 | u8 | `type` |
| 1 | u8 | `player` — sender ID. Clients send 0; the server overwrites it with the real sender ID before relaying, so IDs can't be spoofed. |
| 2 | u16 | `proto` — protocol version (1) |

## Frame types

| Type | Name | Direction | Payload (offset 4..47) |
|---|---|---|---|
| 1 | HELLO | client → server | `room[12]` (4..15), `name[16]` (16..31) — NUL-padded ASCII |
| 2 | WELCOME | server → client | `player` = your ID; `max_players` u8 at 4 |
| 3 | STATE | both | `map` u16 @4, `facing` u16 @6, `x` f32 @8, `z` f32 @12 (height), `y` f32 @16, `aux` u32 @20 (raw u32 from player+0x08, meaning not yet known), `seq` u32 @24, `dirX` f32 @28, `dirY` f32 @32 (unit facing vector in map X/Y; both 0 = unknown, use `facing`), `anim` u8 @36 (sender's animation id from its move model +0x58: 0 idle, 0x10 walk; 0 from older clients), `model` u16 @38 (sender's player model: 0xAC Brendan, 0xAB May; 0 = unknown), `status` u8 @37 (bit 0 busy: under script control for 2+ s (soaring, battle, cutscene) or off the field (then position = last field spot); bit 1 riding a bike, bit 2 surfing), `time` u32 @40 (sender clock in ms, for interpolation; 0 from older clients) |
| 4 | CHAT | both | `text[40]` (4..43) |
| 5 | JOIN | server → client | `player` = who joined; `name[16]` (4..19) |
| 6 | LEAVE | server → client | `player` = who left |
| 7 | ERROR | server → client | `code` u8 @4 (1 = room full, 2 = bad hello, 3 = version mismatch) |
| 8 | PING | client → server | none (keep-alive; not relayed) |
| 9 | GREET | both | `text[40]` (4..43) — what the sender says when someone talks to them. Sent after WELCOME, whenever it changes, and again when someone joins (the server keeps no state). |
| 10 | TALK | both | `target` u8 @4 — the sender pressed A facing player `target`; only the target reacts (a notice). |
| 11 | BATTLE | both | The sender's battle, sent on every change and every 2 s while it lasts: `side` u8 @4 (0 = the sender's team, 1 = the opponent), `count` u8 @5, then `count` (≤ 6) entries of 7 bytes from @6: `species` u16, `hp` u16, `maxHp` u16, `level` u8. Entry 0 is the Pokémon out. `count` 0 = the battle is over. |
| 12 | TRADE | both | One step of a trade, for player `target` u8 @5 only: `op` u8 @4 (1 OFFER, 2 ACCEPT, 3 CANCEL). OFFER: `chunk` u8 @6 of `chunks` u8 @7 (7), 40 bytes of the offered Pokémon @8 (0x104 bytes in all: encrypted 0xE8 data + 0x1C party stats, as the game stores them). ACCEPT: u32 @8 = FNV-1a over both offers (lower player id's first); a game writes the other Pokémon only once it has accepted and received a matching ACCEPT. |

## Flow

1. Client connects and sends HELLO within 5 s.
2. Server replies WELCOME, then sends a JOIN for every player already in the room,
   then broadcasts a JOIN for the new player to everyone else.
3. Clients send STATE about 15×/s while moving and at least once per second while idle.
   The server relays STATE, CHAT, GREET, TALK, BATTLE and TRADE to everyone else in the room.
4. On disconnect, or after 10 s of silence, the server broadcasts LEAVE.

Positions are map-local; clients only draw players whose `map` matches their own.
