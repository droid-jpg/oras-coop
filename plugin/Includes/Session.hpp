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
    char greeting[40] = {0};      // what they say when we talk to them (T_GREET; empty = not received)
    game::BattleState battle;     // their battle while they are in one (T_BATTLE)
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
    // Local player state, read from game memory by the game thread (overlay) each frame and
    // published here; the net thread only reads this copy, never game memory.
    game::PlayerState local;
    game::BattleState localBattle;   // our battle, published by the game thread
    Notice notices[MAX_NOTICES];     // queue, oldest first
    int noticeCount = 0;
    Trade trade;
    Event events[MAX_EVENTS];        // ours, not sent yet (game thread -> net thread)
    int eventCount = 0;
    volatile bool running = true;
};

extern Session g_session;

// Queues a notice; the caller holds g_session.lock.
void PushNotice(const std::string& text, u8 from = 0);

// Hash of both trade offers, the same on both sides (the lower player id's offer first).
u32 TradeHash(const Trade& t, u8 myId);

void NetThread(void*);

// Watches our own game for catches, evolutions and new badges and queues them as events
// (Activity.cpp). Game thread, once per frame; takes the lock itself.
void UpdateActivity(const game::PlayerState& me);

} // namespace coop
