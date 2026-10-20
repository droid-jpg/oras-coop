// State shared between the network thread and the overlay. Guard with Session::lock.
#pragma once
#include <3ds/types.h>
#include <CTRPluginFramework.hpp>
#include <string>
#include "Protocol.hpp"
#include "Game.hpp"

namespace coop {

struct Config {
    std::string server = "127.0.0.1";
    u16 port = 7777;
    std::string room = "HOENN";
    std::string name = "Trainer";
    std::string greeting = "Hi!";  // shown to friends who talk to you (A facing your trainer)
    float pxPerUnit = 1.4f; // overlay scale: screen pixels per world unit (tile = 18 units)
    bool overlay = true;
    bool puppets = false;   // "real models": drive NPCs to remote players (writes game memory)

    void Load();
    void Save() const;
};

// One received STATE, stamped with the sender's clock (ms) for interpolation.
struct Sample {
    u32 t = 0;
    float x = 0, z = 0, y = 0;
    float dirX = 0, dirY = 0;
};

struct Remote {
    static constexpr int HISTORY = 8;
    Sample hist[HISTORY];         // oldest first
    int histLen = 0;
    s64 clockOffset = 0;          // local ms - sender ms (smallest seen, i.e. least delayed)
    bool haveOffset = false;

    bool active = false;
    bool hasState = false;
    char name[16] = {0};
    u16 map = 0;
    u16 facing = 0;
    float tx = 0, tz = 0, ty = 0; // latest received position
    float dx = 0, dz = 0, dy = 0; // smoothed display position
    float dirX = 0, dirY = 0;     // facing unit vector (0,0 = unknown, use facing)
    u8 anim = 0;                  // sender's animation id (0 idle, 0x10 walk; 0 from old clients)
    u16 model = 0;                // sender's player model (0xAC Brendan, 0xAB May; 0 = unknown)
    bool away = false;            // in a battle / soaring / cutscene: not on the field
    bool riding = false;          // on a bike
    bool surfing = false;         // surfing (shown on the game's surf mount)
    bool noWarp = false;          // ST_NO_WARP in their latest STATE
    // Their last map change (T_STATE with a new map), for learning doors/warps (Overlay.cpp):
    // they left `warpFrom` at `fromX/Z/Y` and came out at the first position on `map`.
    u32 warpSeq = 0;              // bumped on every map change
    bool warpOk = false;          // a walk through a door/edge (no ST_NO_WARP either side)
    u16 warpFrom = 0;
    float fromX = 0, fromZ = 0, fromY = 0;
    float arriveX = 0, arriveZ = 0, arriveY = 0;
    char greeting[40] = {0};      // what they say when we talk to them (T_GREET; empty = not received)
    game::BattleState battle;     // their battle while they are in one (T_BATTLE)
    // Their party (T_PARTY), asked for when we talk to them, for battling it.
    u8 party[6][game::PK_SIZE];
    u8 partyCount = 0;
    u64 partyPieces = 0;          // bit per received piece (slot * PARTY_CHUNKS + chunk)
    bool partyComplete = false;
    u64 battleTime = 0;           // when it was last updated
    u64 lastUpdate = 0;
};

enum class NetStatus { Idle, Connecting, Connected, Failed };

// A short message shown in a game-style box at the top of the screen for a few seconds
// (friend joined/left, chat, someone talking to you). Never blocks the game.
struct Notice {
    char text[64] = {0};
    u8 from = 0;          // player id for the colour (0 = neutral)
};
constexpr int MAX_NOTICES = 8;

// One trade at a time, with one partner. Both offer a party Pokemon (the whole encrypted blob
// travels), both accept, then each game writes the other side's Pokemon into its offered slot.
struct Trade {
    u8 partner = 0;
    int mySlot = -1;                       // offered party slot (-1 = not chosen yet)
    u8 mine[game::PK_SIZE];
    u8 theirs[game::PK_SIZE];
    u32 theirChunks = 0;                   // bit per received chunk
    bool theirComplete = false;
    bool sendOffer = false;                // net thread: send `mine`
    bool sendAccept = false, sendCancel = false;
    bool iAccepted = false, theyAccepted = false;
    u32 theirHash = 0;                     // hash their accept was for
    bool pendingWrite = false;             // both accepted: the game thread writes at a safe moment
};

// Something that happened in our game, for friends' notices (T_EVENT): caught, evolved, badge.
struct Event {
    u8 kind = 0, value = 0;
    u16 species = 0, from = 0;
};
constexpr int MAX_EVENTS = 8;

struct Session {
    CTRPluginFramework::Mutex lock;
    Config cfg;
    Remote remotes[MAX_ID + 1]; // index = player ID
    u8 myId = 0;
    NetStatus status = NetStatus::Idle;
    std::string statusText = "Not connected";
    std::string pendingChat;     // set by menu, sent by net thread
    bool reconnect = false;      // menu asks net thread to reconnect
    bool greetChanged = false;   // menu changed cfg.greeting: net thread re-sends it
    u8 pendingTalk = 0;          // overlay: we talked to this player (net thread sends T_TALK)
    bool whoAsked = false;       // "/who" in chat: the overlay lists who's online as notices
    // Local player state, read from game memory by the game thread (overlay) each frame and
    // published here; the net thread only reads this copy, never game memory.
    game::PlayerState local;
    game::BattleState localBattle;   // our battle, published by the game thread
    Notice notices[MAX_NOTICES];     // queue, oldest first
    int noticeCount = 0;
    Trade trade;
    // Our party as the game thread last read it (no eggs), for friends who ask for it.
    u8 localParty[6][game::PK_SIZE];
    int localPartyCount = 0;
    u16 partyAsk = 0;                // bit per player: net thread sends PARTY_REQUEST to them
    u16 partyWanted = 0;             // bit per player who asked for our party
    Event events[MAX_EVENTS];        // ours, not sent yet (game thread -> net thread)
    int eventCount = 0;
    volatile bool running = true;
};

extern Session g_session;

// Tells map changes through doors and map edges apart from Fly, Teleport, Dig, Escape Rope,
// soaring and whiteouts: those come from a menu, a long script or time off the field (a battle).
// Feed it every frame (or loop); NoWarp() = the next map change shouldn't be learned as a warp.
struct WarpGuard {
    static constexpr u64 HOLD_MS = 5000;         // stays set this long after the cause
    static constexpr u64 LONG_SCRIPT_MS = 2000;  // script control longer than a door's
    static constexpr u64 OFF_FIELD_MS = 3000;    // off the field longer than a map load
    u64 until = 0, suspendedSince = 0, lastLive = 0;

    void Update(const game::PlayerState& s, u64 now) {
        bool live = s.valid && s.inField;
        if (live) {
            lastLive = now;
            if (s.suspended) { if (!suspendedSince) suspendedSince = now; }
            else suspendedSince = 0;
            if (s.menuOpen || (suspendedSince && now - suspendedSince >= LONG_SCRIPT_MS)) until = now + HOLD_MS;
        } else if (lastLive && now - lastLive >= OFF_FIELD_MS) {
            until = now + HOLD_MS;
        }
    }
    bool NoWarp(u64 now) const { return now < until; }
};

// Queues a notice; the caller holds g_session.lock.
void PushNotice(const std::string& text, u8 from = 0);

// Hash of both trade offers, the same on both sides (the lower player id's offer first).
u32 TradeHash(const Trade& t, u8 myId);

void NetThread(void*);

// Watches our own game for catches, evolutions and new badges and queues them as events
// (Activity.cpp). Game thread, once per frame; takes the lock itself. quiet: only keep up with
// the game (a friendly battle runs: its level-ups and our restore afterwards aren't news).
void UpdateActivity(const game::PlayerState& me, bool quiet);

} // namespace coop
