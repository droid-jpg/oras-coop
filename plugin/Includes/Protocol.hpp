// Wire format shared with server/server.mjs. See PROTOCOL.md.
#pragma once
#include <3ds/types.h>
#include <cstring>

namespace coop {

constexpr int FRAME = 48;
constexpr u16 PROTO = 1;
constexpr int MAX_ID = 8; // player IDs are 1..8

enum : u8 {
    T_HELLO = 1, T_WELCOME, T_STATE, T_CHAT, T_JOIN, T_LEAVE, T_ERROR, T_PING,
    T_GREET,  // what you say when someone talks to you: text[40] @4 (on connect, on change, on new joins)
    T_TALK,   // talking to player u8 @4 (they get a notice)
    T_BATTLE, // the sender's battle: side u8 @4, count u8 @5, entries @6 (BATTLE_ENTRY each); count 0 = over
    T_TRADE,  // trade step: op u8 @4, target u8 @5, chunk u8 @6, chunks u8 @7, data @8..47
    T_EVENT,  // something happened in the sender's game: kind u8 @4, value u8 @5, species u16 @6, from u16 @8
};

enum : u8 {
    EVENT_CAUGHT = 1,   // species = what they caught
    EVENT_EVOLVED = 2,  // from -> species
    EVENT_BADGE = 3,    // value = badge number (1..8)
};

constexpr int BATTLE_ENTRY = 7; // u16 species, u16 hp, u16 max HP, u8 level

enum : u8 {
    TRADE_OFFER = 1,   // chunk of the offered Pokemon (game::PK_SIZE bytes in TRADE_CHUNK pieces)
    TRADE_ACCEPT = 2,  // agree to swap; data @8 = u32 hash of both offers
    TRADE_CANCEL = 3,
    TRADE_COMMIT = 4,  // from the server, to both players: both accepted the same pair (hash @8); swap now
};
constexpr int TRADE_CHUNK = 40;

// STATE status bits (@37).
enum : u8 {
    ST_AWAY = 1 << 0,   // busy: script control 2+ s (soaring, battle...) or off the field (last spot)
    ST_RIDING = 1 << 1, // on a bike
    ST_SURFING = 1 << 2, // surfing
};

struct Frame {
    u8 b[FRAME];

    explicit Frame(u8 type = 0) {
        std::memset(b, 0, FRAME);
        b[0] = type;
        b[2] = PROTO & 0xFF;
        b[3] = PROTO >> 8;
    }
    u8 type() const { return b[0]; }
    u8 player() const { return b[1]; }

    template <typename T> T get(int off) const { T v; std::memcpy(&v, b + off, sizeof v); return v; }
    template <typename T> void put(int off, T v) { std::memcpy(b + off, &v, sizeof v); }

    void putStr(int off, int len, const char* s) {
        std::strncpy(reinterpret_cast<char*>(b + off), s, len - 1);
    }
    void getStr(int off, int len, char* out) const {
        std::memcpy(out, b + off, len - 1);
        out[len - 1] = 0;
    }
};

} // namespace coop
