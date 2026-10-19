// State shared between the network thread and the overlay. Guard with Session::lock.
#pragma once
#include <3ds/types.h>
#include <CTRPluginFramework.hpp>
#include <string>
#include "Protocol.hpp"

namespace coop {

struct Config {
    std::string server = "127.0.0.1";
    u16 port = 7777;
    std::string room = "HOENN";
    std::string name = "Trainer";
    float pxPerUnit = 1.4f; // overlay scale: screen pixels per world unit (tile = 18 units)
    bool overlay = true;
    bool puppets = false;   // "real models": drive NPCs to remote players (writes game memory)

    void Load();
    void Save() const;
};

struct Remote {
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
    u64 lastUpdate = 0;
};

enum class NetStatus { Idle, Connecting, Connected, Failed };

struct Session {
    CTRPluginFramework::Mutex lock;
    Config cfg;
    Remote remotes[MAX_ID + 1]; // index = player ID
    u8 myId = 0;
    NetStatus status = NetStatus::Idle;
    std::string statusText = "Not connected";
    std::string pendingChat;     // set by menu, sent by net thread
    std::string lastChat;        // shown on screen
    u64 lastChatTime = 0;
    bool reconnect = false;      // menu asks net thread to reconnect
    volatile bool running = true;
};

extern Session g_session;

void NetThread(void*);

} // namespace coop
