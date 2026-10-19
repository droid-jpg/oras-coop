// oras-coop: see friends in Hoenn. Alpha Sapphire 1.4, read-only.
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "Session.hpp"
#include "Game.hpp"

using namespace CTRPluginFramework;

namespace coop {
bool OverlayCallback(const Screen& screen);
}

using coop::g_session;

namespace {

void SaveAndReconnect() {
    g_session.lock.Lock();
    g_session.cfg.Save();
    g_session.reconnect = true;
    g_session.lock.Unlock();
}

bool AskText(const std::string& prompt, std::string& value, int maxLen) {
    Keyboard kb(prompt);
    kb.SetMaxLength(maxLen);
    std::string out = value;
    if (kb.Open(out, value) != 0 || out.empty()) return false;
    value = out;
    return true;
}

void MenuServer(MenuEntry*) {
    g_session.lock.Lock();
    std::string server = g_session.cfg.server;
    u16 port = g_session.cfg.port;
    g_session.lock.Unlock();

    if (!AskText("Server IP (host's LAN or ZeroTier IP):", server, 15)) return;
    std::string portStr = std::to_string(port);
    if (AskText("Server port:", portStr, 5)) port = (u16)std::atoi(portStr.c_str());

    g_session.lock.Lock();
    g_session.cfg.server = server;
    g_session.cfg.port = port ? port : 7777;
    g_session.lock.Unlock();
    SaveAndReconnect();
    OSD::Notify("Server set to " + server);
}

void MenuRoom(MenuEntry*) {
    g_session.lock.Lock();
    std::string room = g_session.cfg.room;
    g_session.lock.Unlock();
    if (!AskText("Room code (everyone uses the same one):", room, 11)) return;
    g_session.lock.Lock();
    g_session.cfg.room = room;
    g_session.lock.Unlock();
    SaveAndReconnect();
}

void MenuName(MenuEntry*) {
    g_session.lock.Lock();
    std::string name = g_session.cfg.name;
    g_session.lock.Unlock();
    if (!AskText("Your name:", name, 15)) return;
    g_session.lock.Lock();
    g_session.cfg.name = name;
    g_session.lock.Unlock();
    SaveAndReconnect();
}

void MenuChat(MenuEntry*) {
    static const char* presets[] = { "Hi!", "Follow me", "Wait here", "Battle?", "Trade?", "GG", "brb" };
    Keyboard kb("Send a message:");
    std::vector<std::string> options = { ">> Type your own <<" };
    for (auto p : presets) options.push_back(p);
    kb.Populate(options);
    int idx = kb.Open();
    std::string text;
    if (idx == 0) {
        if (!AskText("Message:", text, 39)) return;
    } else if (idx > 0) {
        text = presets[idx - 1];
    } else {
        return;
    }
    g_session.lock.Lock();
    g_session.pendingChat = text;
    g_session.lastChat = g_session.cfg.name + ": " + text;
    g_session.lastChatTime = osGetTime();
    g_session.lock.Unlock();
}

// What friends see in a text box when they walk up to your trainer and press A.
void MenuGreeting(MenuEntry*) {
    static const char* presets[] = { "Hi!", "Nice to meet you!", "Want to battle?", "Want to trade?",
                                     "Let's explore together!", "Follow me!", "I'm busy right now." };
    g_session.lock.Lock();
    std::string current = g_session.cfg.greeting;
    g_session.lock.Unlock();
    Keyboard kb("Your greeting (friends see it when they\ntalk to you with A). Now: \"" + current + "\"");
    std::vector<std::string> options = { ">> Type your own <<" };
    for (auto p : presets) options.push_back(p);
    kb.Populate(options);
    int idx = kb.Open();
    std::string text = current;
    if (idx == 0) {
        if (!AskText("Greeting:", text, 39)) return;
    } else if (idx > 0) {
        text = presets[idx - 1];
    } else {
        return;
    }
    g_session.lock.Lock();
    g_session.cfg.greeting = text;
    g_session.cfg.Save();
    g_session.greetChanged = true;
    g_session.lock.Unlock();
    OSD::Notify("Greeting: " + text);
}

void MenuScale(MenuEntry*) {
    static const float scales[] = { 0.8f, 1.0f, 1.2f, 1.4f, 1.6f, 1.8f, 2.0f, 2.4f };
    Keyboard kb("Marker scale (pixels per world unit).\nPick the one where a friend standing\n1 tile away lines up best:");
    std::vector<std::string> options;
    for (float s : scales) { char b[16]; snprintf(b, sizeof b, "%.1f", s); options.push_back(b); }
    kb.Populate(options);
    int idx = kb.Open();
    if (idx < 0) return;
    g_session.lock.Lock();
    g_session.cfg.pxPerUnit = scales[idx];
    g_session.cfg.Save();
    g_session.lock.Unlock();
}

void MenuToggleOverlay(MenuEntry*) {
    g_session.lock.Lock();
    g_session.cfg.overlay = !g_session.cfg.overlay;
    bool on = g_session.cfg.overlay;
    g_session.cfg.Save();
    g_session.lock.Unlock();
    OSD::Notify(on ? "Overlay on" : "Overlay off");
}

void MenuTogglePuppets(MenuEntry*) {
    g_session.lock.Lock();
    bool on = g_session.cfg.puppets;
    g_session.lock.Unlock();
    if (!on && !(MessageBox("Real models (experimental)",
            "Shows friends as real trainers on your map.\n"
            "This spawns extra characters and WRITES game\n"
            "memory. They're removed when it's turned off,\n"
            "but avoid saving while it's on. Enable?", DialogType::DialogYesNo))()) {
        return;
    }
    g_session.lock.Lock();
    g_session.cfg.puppets = !on;
    g_session.cfg.Save();
    g_session.lock.Unlock();
    OSD::Notify(!on ? "Real models on" : "Real models off");
}

void MenuSpawnTest(MenuEntry*) {
    game::Locate();
    game::PlayerState s = game::Read();
    u16 model = game::PlayerModel();
    if (!s.valid || !model) { MessageBox("Spawn test", "Player not found (are you on a field map?)")(); return; }
    u16 tx = (u16)(s.x / 18.0f) + 2, ty = (u16)(s.y / 18.0f);
    u32 mm = game::SpawnCharacter(0xF0, model, tx, ty);
    char buf[128];
    snprintf(buf, sizeof buf, "model 0x%X at tile %u,%u\nresult: %08lX", (unsigned)model, (unsigned)tx,
             (unsigned)ty, (unsigned long)mm);
    MessageBox("Spawn test", buf)();
}

void MenuActionTest(MenuEntry*) {
    static u32 lastCode = 0x13;
    u32 mm = game::FindCharacter(0xF0);
    if (!mm) {
        MenuSpawnTest(nullptr);
        mm = game::FindCharacter(0xF0);
        if (!mm) return;
    }
    Keyboard kc("Action code (hex, 0..103):");
    kc.IsHexadecimal(true);
    u32 code = lastCode;
    if (kc.Open(code, code) != 0) return;
    Keyboard kd("Direction:");
    kd.Populate(std::vector<std::string>{ "0 up", "1 down", "2 left", "3 right", "9 keep facing" });
    int pick = kd.Open();
    if (pick < 0) return;
    u32 dir = pick == 4 ? 9 : (u32)pick;
    lastCode = code;
    bool ok = game::SetAction(mm, (u16)code, (u8)dir);
    OSD::Notify(ok ? Utils::Format("Action 0x%X dir %u started", code, dir) : std::string("Action refused"));
}

void MenuReconnect(MenuEntry*) {
    g_session.lock.Lock();
    g_session.reconnect = true;
    g_session.lock.Unlock();
    OSD::Notify("Reconnecting...");
}

void MenuStatus(MenuEntry*) {
    game::Locate();
    game::PlayerState s = game::Read();
    g_session.lock.Lock();
    std::string net = g_session.statusText;
    std::string cfg = g_session.cfg.server + ":" + std::to_string(g_session.cfg.port) +
                      "  room " + g_session.cfg.room + "  as " + g_session.cfg.name;
    g_session.lock.Unlock();

    char buf[256];
    if (s.valid) {
        snprintf(buf, sizeof buf, "Player object: %08lX\nMap %u  facing %u\nX %.2f  Y %.2f  height %.2f\nTile %d, %d",
                 (unsigned long)game::ObjectAddress(), (unsigned)s.map, (unsigned)s.facing,
                 s.x, s.y, s.z, (int)(s.x / 18.0f), (int)(s.y / 18.0f));
    } else {
        snprintf(buf, sizeof buf, "Player object: not found\n(are you in the overworld?)");
    }
    MessageBox("oras-coop status", net + "\n" + cfg + "\n\n" + buf)();
}

} // namespace

namespace CTRPluginFramework {

void PatchProcess(FwkSettings&) {}

void OnProcessExit() {
    g_session.running = false;
}

int main() {
    g_session.cfg.Load();

    PluginMenu* menu = new PluginMenu("oras-coop", 0, 1, 0,
        "See your friends in Hoenn.\nAlpha Sapphire 1.4. Read-only unless\n\"Real models\" is turned on.");
    menu->SynchronizeWithFrame(true);

    *menu += new MenuEntry("Status", nullptr, MenuStatus);
    *menu += new MenuEntry("Send chat", nullptr, MenuChat);
    *menu += new MenuEntry("Your greeting", nullptr, MenuGreeting,
                           "What friends see when they walk up to you and press A.");

    MenuFolder* setup = new MenuFolder("Setup");
    *setup += new MenuEntry("Server IP / port", nullptr, MenuServer);
    *setup += new MenuEntry("Room code", nullptr, MenuRoom);
    *setup += new MenuEntry("Your name", nullptr, MenuName);
    *setup += new MenuEntry("Reconnect", nullptr, MenuReconnect);
    *menu += setup;

    MenuFolder* display = new MenuFolder("Display");
    *display += new MenuEntry("Show / hide overlay", nullptr, MenuToggleOverlay);
    *display += new MenuEntry("Marker scale", nullptr, MenuScale);
    *display += new MenuEntry("Real models (experimental)", nullptr, MenuTogglePuppets);
    *display += new MenuEntry("Debug: spawn test character", nullptr, MenuSpawnTest);
    *display += new MenuEntry("Debug: action on test character", nullptr, MenuActionTest);
    *menu += display;

    // Lets friends appear as their own trainer (Brendan/May); see Models.cpp.
    game::InstallModelHook();

    OSD::Run(coop::OverlayCallback);

    Thread net = threadCreate(coop::NetThread, nullptr, 0x8000, 0x30, -2, false);

    menu->Run();

    g_session.running = false;
    if (net) {
        threadJoin(net, U64_MAX);
        threadFree(net);
    }
    delete menu;
    return 0;
}

} // namespace CTRPluginFramework
