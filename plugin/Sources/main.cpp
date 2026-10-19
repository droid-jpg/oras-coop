// oras-coop: see friends in Hoenn. Alpha Sapphire 1.4, read-only.
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Session.hpp"
#include "Game.hpp"

using namespace CTRPluginFramework;

namespace coop {
bool OverlayCallback(const Screen& screen);
bool FriendGreeting(u16 charId, char* out, int len);
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
    coop::PushNotice("You: " + text, g_session.myId);
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

// ---- Trading ----
// Each player picks a party Pokemon for the other; once both have offered and both accepted, each
// game swaps its offered Pokemon for the other one (written once back on the plain field).

std::string MonLabel(const u8* pk) {
    u16 species = 0;
    u8 level = 0;
    if (!game::DescribePokemon(pk, species, level)) return "?";
    char nm[16], buf[32];
    if (!game::SpeciesName(species, nm, sizeof nm)) snprintf(nm, sizeof nm, "#%u", (unsigned)species);
    snprintf(buf, sizeof buf, "%s Lv%u", nm, (unsigned)level);
    return buf;
}

// Lets the player pick a party slot and copies it to `pk`. -1 if cancelled or unreadable.
int PickPartySlot(const std::string& title, u8* pk) {
    std::vector<std::string> labels;
    int n = game::PartyCount();
    u8 tmp[game::PK_SIZE];
    for (int i = 0; i < n; i++) labels.push_back(game::ReadPartySlot(i, tmp) ? MonLabel(tmp) : "?");
    if (labels.empty()) { MessageBox("Trade", "Couldn't read your party.")(); return -1; }
    Keyboard kb(title);
    kb.Populate(labels);
    int idx = kb.Open();
    if (idx < 0 || !game::ReadPartySlot(idx, pk)) return -1;
    return idx;
}

void CancelTrade() {
    g_session.lock.Lock();
    if (g_session.trade.partner) g_session.trade.sendCancel = true; // the net thread clears it after sending
    g_session.lock.Unlock();
}

void MenuTrade(MenuEntry*) {
    g_session.lock.Lock();
    coop::Trade t = g_session.trade;
    game::PlayerState me = g_session.local;
    u8 myId = g_session.myId;
    bool connected = g_session.status == coop::NetStatus::Connected;
    std::string names[coop::MAX_ID + 1];
    std::vector<int> ids;
    for (int id = 1; id <= coop::MAX_ID; id++) {
        names[id] = g_session.remotes[id].name;
        if (id != myId && g_session.remotes[id].active) ids.push_back(id);
    }
    g_session.lock.Unlock();

    if (!connected) { MessageBox("Trade", "Not connected to the server.")(); return; }
    if (!me.inField || me.suspended) {
        MessageBox("Trade", "You can only trade while walking around\n(not in a battle or a cutscene).")();
        return;
    }
    std::string them = t.partner ? names[t.partner] : "";
    if (t.partner && t.sendCancel) return;

    // 1. No trade yet: pick a friend and a Pokemon to offer.
    if (!t.partner) {
        if (ids.empty()) { MessageBox("Trade", "Nobody else is online.")(); return; }
        std::vector<std::string> opts;
        for (int id : ids) opts.push_back(names[id]);
        Keyboard kb("Trade with:");
        kb.Populate(opts);
        int pick = kb.Open();
        if (pick < 0) return;
        u8 pk[game::PK_SIZE];
        int slot = PickPartySlot("Offer which Pokemon to " + opts[pick] + "?", pk);
        if (slot < 0) return;
        g_session.lock.Lock();
        coop::Trade& live = g_session.trade;
        bool taken = live.partner && live.partner != ids[pick];
        if (!taken) {
            if (!live.partner) { live = coop::Trade(); live.partner = (u8)ids[pick]; }
            live.mySlot = slot;
            std::memcpy(live.mine, pk, game::PK_SIZE);
            live.sendOffer = true;
        }
        g_session.lock.Unlock();
        if (taken) MessageBox("Trade", "Someone else just offered you a trade.\nOpen Trade again.")();
        else OSD::Notify("Offered " + MonLabel(pk) + " to " + opts[pick]);
        return;
    }

    // 2. They offered first: offer one back, or decline.
    if (t.mySlot < 0) {
        std::string offer = t.theirComplete ? MonLabel(t.theirs) : "a Pokemon";
        Keyboard kb(them + " offers " + offer + ".");
        kb.Populate(std::vector<std::string>{ "Offer one back", "Decline" });
        int pick = kb.Open();
        if (pick == 1) CancelTrade();
        if (pick != 0) return;
        u8 pk[game::PK_SIZE];
        int slot = PickPartySlot("Offer which Pokemon to " + them + "?", pk);
        if (slot < 0) return;
        g_session.lock.Lock();
        coop::Trade& live = g_session.trade;
        if (live.partner == t.partner) {
            live.mySlot = slot;
            std::memcpy(live.mine, pk, game::PK_SIZE);
            live.sendOffer = true;
        }
        g_session.lock.Unlock();
        OSD::Notify("Offered " + MonLabel(pk) + " to " + them);
        return;
    }

    // 3. Waiting for their offer.
    if (!t.theirComplete) {
        Keyboard kb("You offered " + MonLabel(t.mine) + ".\nWaiting for " + them + " to offer one back.");
        kb.Populate(std::vector<std::string>{ "Keep waiting", "Cancel the trade" });
        if (kb.Open() == 1) CancelTrade();
        return;
    }

    // 4. Both offered: accept?
    if (!t.iAccepted) {
        std::string q = "Trade your " + MonLabel(t.mine) + "\nfor " + them + "'s " + MonLabel(t.theirs) + "?";
        if (t.theyAccepted) q += "\n(" + them + " already accepted.)";
        Keyboard kb(q);
        kb.Populate(std::vector<std::string>{ "Yes, trade", "No, cancel" });
        int pick = kb.Open();
        if (pick == 1) { CancelTrade(); return; }
        if (pick != 0) return;
        u8 now[game::PK_SIZE];
        if (!game::ReadPartySlot(t.mySlot, now) || std::memcmp(now, t.mine, game::PK_SIZE) != 0) {
            MessageBox("Trade", "Your party changed since you offered.\nThe trade was cancelled.")();
            CancelTrade();
            return;
        }
        g_session.lock.Lock();
        coop::Trade& live = g_session.trade;
        if (live.partner == t.partner && live.theirComplete) {
            live.iAccepted = true;
            live.sendAccept = true;
            if (live.theyAccepted && live.theirHash == coop::TradeHash(live, myId)) live.pendingWrite = true;
        }
        g_session.lock.Unlock();
        OSD::Notify("Accepted. Close the menu to finish.");
        return;
    }

    // 5. Waiting for them to accept.
    Keyboard kb("Waiting for " + them + " to accept\n" + MonLabel(t.mine) + " for " + MonLabel(t.theirs) + ".");
    kb.Populate(std::vector<std::string>{ "Keep waiting", "Cancel the trade" });
    if (kb.Open() == 1) CancelTrade();
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
    *menu += new MenuEntry("Trade", nullptr, MenuTrade,
                           "Trade a party Pokemon with a friend. Both pick one, both accept.");
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
    // Talking to friends opens the game's own text box.
    game::InstallTalkHooks(coop::FriendGreeting);

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
