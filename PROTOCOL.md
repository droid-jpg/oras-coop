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
| 3 | STATE | both | `map` u16 @4, `facing` u16 @6, `x` f32 @8, `z` f32 @12 (height), `y` f32 @16, `aux` u32 @20 (raw u32 from player+0x08, meaning not yet known), `seq` u32 @24, `dirX` f32 @28, `dirY` f32 @32 (unit facing vector in map X/Y; both 0 = unknown, use `facing`), `anim` u8 @36 (sender's animation id from its move model +0x58: 0 idle, 0x10 walk; 0 from older clients), `model` u16 @38 (sender's player model: 0xAC Brendan, 0xAB May; 0 = unknown) |
| 4 | CHAT | both | `text[40]` (4..43) |
| 5 | JOIN | server → client | `player` = who joined; `name[16]` (4..19) |
| 6 | LEAVE | server → client | `player` = who left |
| 7 | ERROR | server → client | `code` u8 @4 (1 = room full, 2 = bad hello, 3 = version mismatch) |
| 8 | PING | client → server | none (keep-alive; not relayed) |

## Flow

1. Client connects and sends HELLO within 5 s.
2. Server replies WELCOME, then sends a JOIN for every player already in the room,
   then broadcasts a JOIN for the new player to everyone else.
3. Clients send STATE about 15×/s while moving and at least once per second while idle.
   The server relays STATE and CHAT to everyone else in the room.
4. On disconnect, or after 10 s of silence, the server broadcasts LEAVE.

Positions are map-local; clients only draw players whose `map` matches their own.
