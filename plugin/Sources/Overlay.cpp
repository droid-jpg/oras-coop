// On-screen display: markers for nearby players (top) and a radar + player list (bottom).
#include <3ds.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "Session.hpp"
#include "Game.hpp"
#include "Puppet.hpp"

using namespace CTRPluginFramework;

namespace coop {

namespace {

constexpr int TOP_W = 400, TOP_H = 240, BOT_W = 320, BOT_H = 240;
constexpr int PLAYER_SX = 200, PLAYER_SY = 148; // fallback: where our character's feet sit
constexpr float HEAD_HEIGHT = 30.0f;            // world units from feet to just above the head
constexpr float TILE = 18.0f;
constexpr float RADAR_RANGE = 20.0f * TILE;     // world units from centre to radar edge
constexpr u64 STALE_MS = 5000;
constexpr u64 NOTICE_MS = 3500;       // each notice stays this long (paused while the game's UI is up)
constexpr u64 BATTLE_FRESH_MS = 6000; // a friend's battle info is shown while this recent

const Color kBg(8, 14, 24, 170);
const Color kAccent(0, 210, 255, 220);
const Color kOk(0, 230, 120, 230);
const Color kBad(230, 60, 60, 230);
const Color kClear(0, 0, 0, 0);

const Color kPlayerColors[MAX_ID + 1] = {
    Color(255, 255, 255), Color(255, 90, 90), Color(90, 170, 255), Color(255, 210, 60), Color(120, 230, 120),
    Color(220, 120, 255), Color(255, 150, 60), Color(90, 230, 220), Color(255, 130, 200),
};

// Clipped rectangle so nothing is ever drawn outside the framebuffer.
void Rect(const Screen& s, int x, int y, int w, int h, const Color& c, bool fill = true) {
    int maxW = s.IsTop ? TOP_W : BOT_W, maxH = s.IsTop ? TOP_H : BOT_H;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > maxW) w = maxW - x;
    if (y + h > maxH) h = maxH - y;
    if (w <= 0 || h <= 0) return;
    s.DrawRect(x, y, w, h, c, fill);
}

void Text(const Screen& s, const std::string& t, int x, int y, const Color& c) {
    int maxW = s.IsTop ? TOP_W : BOT_W, maxH = s.IsTop ? TOP_H : BOT_H;
    if (x < 0 || y < 0 || y + 10 > maxH || x >= maxW) return;
    s.Draw(t, x, y, c, kClear);
}

int TextWidth(const std::string& t) { return (int)t.size() * 6; }

float Clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

struct View {
    game::PlayerState me;
    Remote remotes[MAX_ID + 1];
    u8 myId;
    NetStatus status;
    std::string statusText, room, name;
    Notice notice;      // front of the notice queue
    bool hasNotice;
    float scale;
    bool overlay;
    bool puppets;
};

// Remote players are shown INTERP_DELAY_MS in the past, between the two received states
// around that moment, so movement keeps a constant speed despite 15 Hz updates and jitter.
// Must exceed the sender's update interval (66 ms) plus typical jitter.
constexpr u32 INTERP_DELAY_MS = 120;

void Interpolate(Remote& r, u64 now, float& dirX, float& dirY) {
    if (r.histLen == 0 || !r.haveOffset) {
        r.dx = r.tx; r.dz = r.tz; r.dy = r.ty;
        return;
    }
    u32 t = (u32)((s64)(u32)now - r.clockOffset) - INTERP_DELAY_MS; // in sender ms
    const Sample* a = &r.hist[0];
    const Sample* b = nullptr;
    for (int k = 1; k < r.histLen; k++) {
        if ((s32)(r.hist[k].t - t) >= 0) { b = &r.hist[k]; break; }
        a = &r.hist[k];
    }
    if (!b || (s32)(t - a->t) < 0) {
        // Past the newest state (late packet or standing still) or before the oldest: hold.
        // No extrapolation — overshooting and snapping back looks worse than a short pause.
        r.dx = a->x; r.dz = a->z; r.dy = a->y;
        dirX = a->dirX; dirY = a->dirY;
        return;
    }
    float span = (float)(s32)(b->t - a->t);
    float u = span > 0 ? (float)(s32)(t - a->t) / span : 1.0f;
    if (std::fabs(b->x - a->x) > 10 * TILE || std::fabs(b->y - a->y) > 10 * TILE) u = 1.0f; // warp
    r.dx = a->x + (b->x - a->x) * u;
    r.dz = a->z + (b->z - a->z) * u;
    r.dy = a->y + (b->y - a->y) * u;
    // Facing: blend the vectors (renormalised by Puppet); fall back if one is unknown.
    if ((a->dirX || a->dirY) && (b->dirX || b->dirY)) {
        dirX = a->dirX + (b->dirX - a->dirX) * u;
        dirY = a->dirY + (b->dirY - a->dirY) * u;
        if (dirX * dirX + dirY * dirY < 1e-4f) { dirX = b->dirX; dirY = b->dirY; } // 180° turn
    } else {
        dirX = b->dirX; dirY = b->dirY;
    }
}

// Copies shared state and advances smoothing; done once per top-screen frame.
View Snapshot() {
    View v;
    game::Locate(); // cheap when found; otherwise scans a small slice of the heap
    v.me = game::Read();
    game::BattleState battle;
    game::ReadBattle(battle);
    g_session.lock.Lock();
    g_session.local = v.me;
    g_session.localBattle = battle;
    u64 now = osGetTime();
    for (int i = 0; i <= MAX_ID; i++) {
        Remote& r = g_session.remotes[i];
        float dirX = r.dirX, dirY = r.dirY;
        if (r.hasState) Interpolate(r, now, dirX, dirY);
        v.remotes[i] = r;
        v.remotes[i].dirX = dirX; // display facing; the shared copy keeps the latest received
        v.remotes[i].dirY = dirY;
    }
    v.myId = g_session.myId;
    v.status = g_session.status;
    v.statusText = g_session.statusText;
    v.hasNotice = g_session.noticeCount > 0;
    if (v.hasNotice) v.notice = g_session.notices[0];
    v.room = g_session.cfg.room;
    v.name = g_session.cfg.name;
    v.scale = g_session.cfg.pxPerUnit;
    v.overlay = g_session.cfg.overlay;
    v.puppets = g_session.cfg.puppets;
    g_session.lock.Unlock();
    return v;
}

View g_view;

bool Visible(const Remote& r, const game::PlayerState& me) {
    return r.active && r.hasState && me.valid && me.inField && !me.suspended && r.map == me.map &&
           osGetTime() - r.lastUpdate < STALE_MS;
}

// The game's place name for a map ("Route 109"; buildings give their town), or "map N" until
// the place-name text has been found.
std::string PlaceOf(u16 map) {
    char buf[32];
    if (game::PlaceName(map, buf, sizeof buf)) return buf;
    snprintf(buf, sizeof buf, "map %u", (unsigned)map);
    return buf;
}

// "Wingull Lv20" (the game's species name when it has been found).
std::string MonName(u16 species, u8 level) {
    char nm[16], buf[32];
    if (!game::SpeciesName(species, nm, sizeof nm)) snprintf(nm, sizeof nm, "#%u", (unsigned)species);
    snprintf(buf, sizeof buf, "%s Lv%u", nm, (unsigned)level);
    return buf;
}

bool InBattle(const Remote& r) {
    return r.away && r.battle.active && osGetTime() - r.battleTime < BATTLE_FRESH_MS;
}

// "Wingull Lv20", or "Bianca (Azelf Lv100)" when it has a nickname, for a party Pokemon's data.
std::string MonLabel(const u8* pk) {
    u16 species = 0;
    u8 level = 0;
    char nick[16] = {0}, sp[16] = {0};
    if (!game::DescribePokemon(pk, species, level, nick, sizeof nick)) return "a Pokemon";
    std::string plain = MonName(species, level);
    if (!nick[0] || (game::SpeciesName(species, sp, sizeof sp) && std::strcmp(sp, nick) == 0)) return plain;
    return std::string(nick) + " (" + plain + ")";
}

// The YesNoPick list: our party, for choosing what to offer.
void FillPartyChoices(game::Dialog& d) {
    snprintf(d.pick, sizeof d.pick, "Which Pokemon will you offer?");
    d.itemCount = 0;
    u8 pk[game::PK_SIZE];
    for (int i = 0; i < game::PartyCount() && i < game::DIALOG_ITEMS; i++) {
        snprintf(d.items[i], sizeof d.items[i], "%s", game::ReadPartySlot(i, pk) ? MonLabel(pk).c_str() : "?");
        d.itemCount = i + 1;
    }
}

// ---- What friends say: greetings, their battle, and trades ----
// Talking to a friend (A in front of their trainer) opens a dialog in the game's own text box.
// Trading runs entirely through it: "Trade Pokemon with X?" Yes -> the game's party screen ->
// your offer goes to X. X talks to you -> "X offers ..." Yes -> picks one back, which also
// accepts. You talk to X again -> "Trade your A for X's B?" Yes. The server then confirms both
// accepts to both games at once (COMMIT), and each swaps in the other Pokemon.
enum class Purpose : u8 { None, Menu, Offer, AnswerOffer, CancelWaiting, Accept };
struct DialogCtx {
    int id = 0;
    Purpose purpose = Purpose::None;
    int battleAction = -1, tradeAction = -1;  // Menu
};
DialogCtx g_dialog;   // the dialog running now (results arrive on the game thread while it runs)

// ---- Battling a friend's team ----
// "Battle" in the talk menu arms FriendBattle.cpp with their party (asked for when we started
// talking) and our script starts a trainer battle against the rival that looks like them. It's a
// friendly battle: afterwards our party (HP, experience...) and money are put back as they were.
struct FriendFight {
    bool armed = false;      // waiting for the battle to start (or to be given up)
    bool running = false;    // their team is in: restore once we're back on the plain field
    int id = 0;
    u64 armedAt = 0;
    u8 saved[6][game::PK_SIZE];
    int savedCount = 0;
    u32 money = 0;
    bool haveMoney = false;
    game::BattleState last;  // the battle as last seen, to tell who won
};
FriendFight g_fight;
constexpr u64 FIGHT_START_MS = 20000; // armed but no battle by then: the dialog went another way

bool StartFriendFight(int id) {
    g_session.lock.Lock();
    const Remote& r = g_session.remotes[id];
    bool ok = r.active && r.partyComplete && r.partyCount > 0;
    static u8 party[6][game::PK_SIZE];
    int count = r.partyCount;
    char name[sizeof r.name];
    std::memcpy(name, r.name, sizeof name);
    if (ok) std::memcpy(party, r.party, sizeof party);
    g_session.lock.Unlock();
    if (!ok) return false;
    g_fight = FriendFight();
    // Our script starts the battle either way: their team goes in even if ours can't be saved.
    g_fight.savedCount = game::PartyCount();
    for (int i = 0; i < g_fight.savedCount; i++) {
        if (!game::ReadPartySlot(i, g_fight.saved[i])) { g_fight.savedCount = -1; break; }
    }
    g_fight.haveMoney = game::ReadMoney(g_fight.money);
    g_fight.id = id;
    g_fight.armed = true;
    g_fight.armedAt = osGetTime();
    game::ArmFriendBattle(&party[0][0], count, name);
    return true;
}

bool AllDown(const game::BattleState& b, int side) {
    if (!b.count[side]) return false;
    for (int i = 0; i < b.count[side]; i++) if (b.mon[side][i].hp) return false;
    return true;
}

void UpdateFriendFight(const View& v) {
    if (g_fight.armed && game::FriendBattleStarted()) { g_fight.armed = false; g_fight.running = true; }
    if (g_fight.armed && osGetTime() - g_fight.armedAt > FIGHT_START_MS) {
        game::DisarmFriendBattle();
        g_fight = FriendFight();
        return;
    }
    if (!g_fight.running) return;
    game::BattleState b;
    if (game::ReadBattle(b)) { g_fight.last = b; return; }
    // Back on the plain field: put everything back.
    if (!v.me.inField || v.me.suspended || v.me.locked || v.me.menuOpen || !game::FieldModuleLoaded() ||
        game::GameScriptRunning())
        return;
    bool restored = game::PartyCount() == g_fight.savedCount;
    for (int i = 0; restored && i < g_fight.savedCount; i++) restored = game::WritePartySlot(i, g_fight.saved[i]);
    if (g_fight.haveMoney) game::WriteMoney(g_fight.money);
    game::DisarmFriendBattle();
    g_session.lock.Lock();
    std::string name = g_session.remotes[g_fight.id].name;
    const char* how = AllDown(g_fight.last, 1) ? "You beat " : AllDown(g_fight.last, 0) ? "You lost to " : "Battle over: ";
    PushNotice(std::string(how) + name + "'s team!" + (restored ? "" : " (party changed, not restored)"), (u8)g_fight.id);
    g_session.lock.Unlock();
    g_fight = FriendFight();
}

game::Dialog FriendDialog(int id) {
    game::Dialog d;
    g_dialog = DialogCtx();
    g_dialog.id = id;
    g_session.lock.Lock();
    const Remote r = g_session.remotes[id];
    const Trade t = g_session.trade;
    g_session.lock.Unlock();
    std::string name = r.name, greeting = r.greeting[0] ? r.greeting : "...";
    auto set = [](char* dst, const std::string& s) { snprintf(dst, 96, "%s", s.c_str()); };

    if (InBattle(r)) {
        const game::BattleMon &a = r.battle.mon[0][0], &b = r.battle.mon[1][0];
        set(d.intro, name + " is in a battle!");
        char buf[96];
        snprintf(buf, sizeof buf, "%s HP %u/%u\nvs %s HP %u/%u", MonName(a.species, a.level).c_str(),
                 (unsigned)a.hp, (unsigned)a.maxHp, MonName(b.species, b.level).c_str(), (unsigned)b.hp,
                 (unsigned)b.maxHp);
        set(d.text, buf);
        return d;
    }
    if (t.partner && t.partner != id) { // trading with someone else: just chat
        set(d.text, greeting);
        return d;
    }
    if (!t.partner) {
        set(d.intro, greeting);
        set(d.text, "What do you want to do?");
        d.kind = game::DialogKind::Menu;
        auto action = [&](const char* label) {
            snprintf(d.actions[d.actionCount], sizeof d.actions[0], "%s", label);
            return d.actionCount++;
        };
        if (r.partyComplete && r.partyCount > 0) { // asked for when we last talked to them
            d.battleAction = action("Battle");
            d.battleTrainer = game::IsMayModel(r.model) ? game::TRAINER_MAY : game::TRAINER_BRENDAN;
        }
        d.tradeAction = action("Trade");
        action("Cancel");
        FillPartyChoices(d);
        g_dialog.purpose = Purpose::Menu;
        g_dialog.battleAction = d.battleAction;
        g_dialog.tradeAction = d.tradeAction;
    } else if (t.mySlot < 0) {
        if (!t.theirComplete) { set(d.text, greeting); return d; } // their offer is still arriving
        set(d.intro, name + " offers " + MonLabel(t.theirs) + ".");
        set(d.text, "Offer one of yours for it?");
        d.kind = game::DialogKind::YesNoPick;
        FillPartyChoices(d);
        g_dialog.purpose = Purpose::AnswerOffer;
    } else if (!t.theirComplete) {
        set(d.intro, "You offered " + MonLabel(t.mine) + ".\nWaiting for " + name + " to pick one.");
        set(d.text, "Call off the trade?");
        d.kind = game::DialogKind::YesNo;
        g_dialog.purpose = Purpose::CancelWaiting;
    } else if (!t.iAccepted) {
        set(d.intro, "You give: " + MonLabel(t.mine) + "\nYou get: " + MonLabel(t.theirs));
        set(d.text, "Trade with " + name + "?");
        d.kind = game::DialogKind::YesNo;
        g_dialog.purpose = Purpose::Accept;
    } else {
        set(d.text, "Waiting for " + name + " to confirm\nthe trade...");
    }
    return d;
}

// The game's Yes/No answer as the Surf script reads it: non-zero = Yes.
bool IsYes(int answer) { return answer != 0; }

// Acts on an answer in the running dialog and writes the reply line (game thread).
void OnDialogResult(game::DialogResult what, int value, char* reply, int len) {
    const DialogCtx ctx = g_dialog;
    g_dialog.purpose = Purpose::None; // one answer per dialog
    g_session.lock.Lock();
    Trade& t = g_session.trade;
    std::string name = ctx.id >= 1 && ctx.id <= MAX_ID ? g_session.remotes[ctx.id].name : "them";
    std::string text = "Maybe next time.";
    bool partnerOk = t.partner == 0 || t.partner == ctx.id;
    switch (ctx.purpose) {
    case Purpose::Menu:
        if (what != game::DialogResult::Action) break;
        if (value == ctx.battleAction) {
            g_session.lock.Unlock();
            bool ok = StartFriendFight(ctx.id);
            g_session.lock.Lock();
            text = ok ? "Let's battle, " + name + "!" : name + "'s team isn't here yet...";
        } else if (value == ctx.tradeAction) {
            g_dialog.purpose = Purpose::Offer; // the party list follows
        }
        break;
    case Purpose::Offer:
    case Purpose::AnswerOffer: {
        if (what == game::DialogResult::Answer) { // said No
            if (ctx.purpose == Purpose::AnswerOffer && t.partner == ctx.id) {
                t.sendCancel = true;
                text = "You turned down the trade.";
            }
            break;
        }
        u8 pk[game::PK_SIZE];
        if (value >= game::PartyCount() || !game::ReadPartySlot(value, pk)) break; // backed out
        if (!partnerOk) { text = "Someone else is trading with you."; break; }
        if (ctx.purpose == Purpose::AnswerOffer && !(t.partner == ctx.id && t.theirComplete)) {
            text = name + " called off the trade.";
            break;
        }
        if (!t.partner) { t = Trade(); t.partner = (u8)ctx.id; }
        t.mySlot = value;
        std::memcpy(t.mine, pk, game::PK_SIZE);
        t.sendOffer = true;
        if (ctx.purpose == Purpose::AnswerOffer) {
            // Picking one back means "yes, this for that".
            t.iAccepted = true;
            t.sendAccept = true;
            text = "You offered " + MonLabel(pk) + ".\nWaiting for " + name + " to confirm.";
        } else {
            text = "You offered " + MonLabel(pk) + ".\nWaiting for " + name + " to pick one.";
        }
        break;
    }
    case Purpose::CancelWaiting:
        if (IsYes(value) && t.partner == ctx.id) { t.sendCancel = true; text = "You called off the trade."; }
        else text = "OK, let's wait.";
        break;
    case Purpose::Accept: {
        if (!IsYes(value)) {
            if (t.partner == ctx.id) t.sendCancel = true;
            text = "You called off the trade.";
            break;
        }
        u8 now[game::PK_SIZE];
        if (t.partner != ctx.id || !t.theirComplete) { text = name + " called off the trade."; break; }
        if (!game::ReadPartySlot(t.mySlot, now) || std::memcmp(now, t.mine, game::PK_SIZE) != 0) {
            t.sendCancel = true;
            text = "Your party changed, so the\ntrade was called off.";
            break;
        }
        t.iAccepted = true;
        t.sendAccept = true;
        text = "Trading with " + name + "...";
        break;
    }
    case Purpose::None:
        text = "...";
        break;
    }
    g_session.lock.Unlock();
    snprintf(reply, len, "%s", text.c_str());
}

// ---- Talking to a friend ----
// Walk up to a friend's trainer and press A: their greeting opens in a text box styled like the
// game's, the way talking to an NPC does. A/B closes it; walking away or any game UI too.
constexpr float TALK_RANGE = 1.6f * TILE;     // the tile in front of us (diagonals included)
constexpr float TALK_LEAVE = 2.6f * TILE;     // closes once this far apart
constexpr float TALK_FACING = 0.7f;           // cos of the widest angle that counts as "facing"
constexpr float TALK_CHARS_PER_MS = 0.06f;    // typewriter speed (60 characters/s)
constexpr u64 TALK_MAX_MS = 15000;

struct Talk {
    bool open = false;
    bool native = false;   // shown in the game's own text box (Talk.cpp), not drawn by us
    bool seenScript = false;
    int id = 0;
    u64 start = 0;
    std::string name, text;
};
Talk g_talk;
u32 g_prevKeys = 0;
u64 g_talkCooldown = 0;   // the A that closes the game's box mustn't reopen it
constexpr u64 NATIVE_START_MS = 1500;  // our script must be running by then
constexpr u64 TALK_COOLDOWN_MS = 400;

// The friend we're facing and next to (nearest first), or 0.
int FacingFriend(const View& v) {
    float fx = v.me.dirX, fy = v.me.dirY;
    if (fx == 0 && fy == 0) game::FacingToDir(v.me.facing, fx, fy);
    int best = 0;
    float bestD = TALK_RANGE;
    for (int id = 1; id <= MAX_ID; id++) {
        const Remote& r = v.remotes[id];
        if (id == v.myId || !Visible(r, v.me)) continue;
        float dx = r.dx - v.me.x, dy = r.dy - v.me.y, dz = r.dz - v.me.z;
        float d = std::sqrt(dx * dx + dy * dy);
        if (d < 1.0f || d > bestD || std::fabs(dz) > TILE) continue;
        if ((dx * fx + dy * fy) / d < TALK_FACING) continue;
        best = id;
        bestD = d;
    }
    return best;
}

void UpdateTalk(const View& v) {
    u32 keys = Controller::GetKeysDown(false);
    u32 pressed = keys & ~g_prevKeys;
    g_prevKeys = keys;
    u64 now = osGetTime();
    if (g_talk.open && g_talk.native) {
        // The game runs the box: it's over once our script has run and finished.
        bool running = game::GameScriptRunning();
        if (running) g_talk.seenScript = true;
        if ((g_talk.seenScript && !running) || (!g_talk.seenScript && now - g_talk.start > NATIVE_START_MS) ||
            now - g_talk.start > 60000) {
            g_talk.open = false;
            g_talkCooldown = now + TALK_COOLDOWN_MS;
        }
        return;
    }
    if (g_talk.open) {
        const Remote& r = v.remotes[g_talk.id];
        float dx = r.dx - v.me.x, dy = r.dy - v.me.y;
        bool typed = (now - g_talk.start) * TALK_CHARS_PER_MS >= g_talk.text.size();
        if (!Visible(r, v.me) || v.me.locked || v.me.menuOpen || dx * dx + dy * dy > TALK_LEAVE * TALK_LEAVE ||
            now - g_talk.start > TALK_MAX_MS) {
            g_talk.open = false;
        } else if (pressed & (Key::A | Key::B)) {
            if (typed) g_talk.open = false;
            else g_talk.start = now - (u64)(g_talk.text.size() / TALK_CHARS_PER_MS) - 1; // show it all
        }
        return;
    }
    if (!(pressed & Key::A) || now < g_talkCooldown || !v.overlay || !v.me.inField || v.me.suspended ||
        v.me.locked || v.me.menuOpen)
        return;
    int id = FacingFriend(v);
    // A friend shown by a real trainer is talked to like an NPC: the game starts its talk event
    // and Talk.cpp's hook asks FriendGreeting() for the line. Only marker-only friends need us here.
    if (!id || IsPuppeted(id)) return;
    const Remote& r = v.remotes[id];
    g_talk = Talk();
    g_talk.id = id;
    g_talk.start = now;
    g_talk.name = r.name;
    game::Dialog d = FriendDialog(id);
    g_talk.text = d.text;
    // The game's own text box when it can start one right now; our drawn copy (text only) otherwise.
    int res = game::ShowGameDialog(d);
    if (res == 0) return; // the game is busy this frame (another script): ignore the press
    g_talk.open = true;
    g_talk.native = res == 1;
    g_session.lock.Lock();
    g_session.pendingTalk = (u8)id;
    g_session.partyAsk |= (u16)(1 << id); // fresh for the battle option (it arrives in well under a second)
    g_session.lock.Unlock();
}

// Splits text into at most two lines of maxChars, breaking at spaces where possible.
void Wrap(const std::string& t, size_t maxChars, std::string& l1, std::string& l2) {
    if (t.size() <= maxChars) { l1 = t; l2.clear(); return; }
    size_t cut = t.rfind(' ', maxChars);
    if (cut == std::string::npos || cut == 0) cut = maxChars;
    l1 = t.substr(0, cut);
    l2 = t.substr(cut + (t[cut] == ' ' ? 1 : 0), maxChars);
}

// The game's text box: white panel with a slate border near the bottom of the top screen, the
// speaker's name in a tab above it, text typed out, and a blinking red arrow once it's all shown.
void DrawTalkBox(const Screen& s, const Talk& t, const Color& accent) {
    constexpr int X = 12, Y = 180, W = 376, H = 52;
    const Color border(84, 92, 110), panel(250, 250, 250), line(206, 214, 228), ink(56, 56, 64);
    u64 elapsed = osGetTime() - t.start;
    size_t shown = (size_t)(elapsed * TALK_CHARS_PER_MS);
    if (shown > t.text.size()) shown = t.text.size();

    // Name tab.
    int tw = TextWidth(t.name) + 14;
    Rect(s, X + 8, Y - 13, tw, 2, border);
    Rect(s, X + 6, Y - 11, 2, 12, border);
    Rect(s, X + 8 + tw, Y - 11, 2, 12, border);
    Rect(s, X + 8, Y - 11, tw, 12, panel);
    Rect(s, X + 10, Y - 9, 3, 8, accent);
    Text(s, t.name, X + 16, Y - 10, ink);

    // Panel with slightly rounded corners (the corner pixels are left out of the border).
    Rect(s, X + 2, Y, W - 4, 2, border);
    Rect(s, X + 2, Y + H - 2, W - 4, 2, border);
    Rect(s, X, Y + 2, 2, H - 4, border);
    Rect(s, X + W - 2, Y + 2, 2, H - 4, border);
    Rect(s, X + 1, Y + 1, 1, 1, border);
    Rect(s, X + W - 2, Y + 1, 1, 1, border);
    Rect(s, X + 1, Y + H - 2, 1, 1, border);
    Rect(s, X + W - 2, Y + H - 2, 1, 1, border);
    Rect(s, X + 2, Y + 2, W - 4, H - 4, panel);
    Rect(s, X + 4, Y + 4, W - 8, H - 8, line, false);

    std::string l1, l2;
    Wrap(t.text.substr(0, shown), (W - 32) / 6, l1, l2);
    Text(s, l1, X + 14, Y + 12, ink);
    if (!l2.empty()) Text(s, l2, X + 14, Y + 28, ink);

    if (shown == t.text.size() && (elapsed / 400) % 2 == 0) {
        // Down arrow, like the game's "more" marker.
        const Color red(226, 70, 70);
        for (int i = 0; i < 4; i++) Rect(s, X + W - 22 + i, Y + H - 16 + i, 9 - 2 * i, 1, red);
    }
}

// ---- Friends leaving / arriving through doors and map edges ----
// A friend who leaves our map vanishes on the spot; a short fading note there says where they
// went, and one at their arrival spot says they came in.
constexpr u64 NOTE_MS = 2200;

struct Note {
    u64 start = 0;
    float x = 0, z = 0, y = 0;
    u16 map = 0;
    std::string text;
};
Note g_notes[MAX_ID + 1];
u16 g_lastMap[MAX_ID + 1];
bool g_wasHere[MAX_ID + 1];
float g_lastX[MAX_ID + 1], g_lastZ[MAX_ID + 1], g_lastY[MAX_ID + 1];

void TrackDoors(const View& v) {
    static bool init = false;
    if (!init) { for (auto& m : g_lastMap) m = 0xFFFF; init = true; }
    u64 now = osGetTime();
    for (int id = 1; id <= MAX_ID; id++) {
        const Remote& r = v.remotes[id];
        bool known = r.active && r.hasState;
        bool here = id != v.myId && known && v.me.valid && v.me.inField && r.map == v.me.map;
        if (known && g_wasHere[id] && !here && r.map != g_lastMap[id] && g_lastMap[id] == v.me.map) {
            Note& n = g_notes[id];
            n = Note();
            n.start = now;
            n.x = g_lastX[id]; n.z = g_lastZ[id]; n.y = g_lastY[id];
            n.map = v.me.map;
            n.text = std::string(r.name) + " -> " + PlaceOf(r.map);
        } else if (here && !g_wasHere[id] && g_lastMap[id] != 0xFFFF && g_lastMap[id] != r.map) {
            Note& n = g_notes[id];
            n = Note();
            n.start = now;
            n.x = r.dx; n.z = r.dz; n.y = r.dy;
            n.map = v.me.map;
            n.text = std::string(r.name) + " arrived";
        }
        if (here) { g_lastX[id] = r.dx; g_lastZ[id] = r.dz; g_lastY[id] = r.dy; }
        g_wasHere[id] = here;
        g_lastMap[id] = known ? r.map : 0xFFFF;
    }
}

// ---- Doors and warps: "Name is inside" ----
// Every map change seen (ours, and every friend's, on any map) teaches a warp: map A at spot P
// leads to map B, and B at the arrival spot leads back to A. A friend on another map then gets a
// steady label on the door/edge of our map that leads toward them ("Bot is inside" when the place
// has the same name, i.e. a building in this town; else "Bot is in Granite Cave"). Fly, Escape
// Rope, whiteouts and the like aren't warps: the mover's WarpGuard flags those (ST_NO_WARP).
constexpr int MAX_WARPS = 64;
constexpr float WARP_SAME = 2.5f * TILE;   // one door: two learned spots this close are merged
constexpr int WARP_DEPTH = 4;              // a friend up to this many doors away (house 2F...)

struct Warp { u16 from = 0, to = 0; float x = 0, z = 0, y = 0; u32 used = 0; };
Warp g_warps[MAX_WARPS];
int g_warpCount = 0;
u32 g_warpClock = 0;
u32 g_warpSeen[MAX_ID + 1];
WarpGuard g_myGuard;
u16 g_myMap = 0xFFFF;
float g_myX = 0, g_myZ = 0, g_myY = 0;
bool g_myNoWarp = false;

void LearnWarp(u16 from, u16 to, float x, float z, float y) {
    if (from == to || !std::isfinite(x) || !std::isfinite(y)) return;
    Warp* w = nullptr;
    for (int i = 0; i < g_warpCount && !w; i++) {
        Warp& o = g_warps[i];
        if (o.from == from && o.to == to && std::fabs(o.x - x) < WARP_SAME && std::fabs(o.y - y) < WARP_SAME) w = &o;
    }
    if (!w && g_warpCount < MAX_WARPS) w = &g_warps[g_warpCount++];
    if (!w) { // full: replace the least recently learned
        w = &g_warps[0];
        for (int i = 1; i < MAX_WARPS; i++) if (g_warps[i].used < w->used) w = &g_warps[i];
    }
    w->from = from; w->to = to; w->x = x; w->z = z; w->y = y;
    w->used = ++g_warpClock;
}

void LearnBoth(u16 from, float fx, float fz, float fy, u16 to, float tx, float tz, float ty) {
    LearnWarp(from, to, fx, fz, fy);
    LearnWarp(to, from, tx, tz, ty);
}

void TrackWarps(const View& v) {
    u64 now = osGetTime();
    g_myGuard.Update(v.me, now);
    if (v.me.valid && v.me.inField) {
        // A new map: we walked from where we last stood on the old one (unless the guard was set
        // there: Fly from a menu, a long script, time off the field).
        if (g_myMap != 0xFFFF && v.me.map != g_myMap && !g_myNoWarp)
            LearnBoth(g_myMap, g_myX, g_myZ, g_myY, v.me.map, v.me.x, v.me.z, v.me.y);
        g_myMap = v.me.map;
        g_myX = v.me.x; g_myZ = v.me.z; g_myY = v.me.y;
        g_myNoWarp = g_myGuard.NoWarp(now);
    }
    for (int id = 1; id <= MAX_ID; id++) {
        const Remote& r = v.remotes[id];
        if (!r.active) { g_warpSeen[id] = 0; continue; }
        if (r.warpSeq == g_warpSeen[id]) continue;
        g_warpSeen[id] = r.warpSeq;
        if (r.warpOk && id != v.myId) LearnBoth(r.warpFrom, r.fromX, r.fromZ, r.fromY, r.map, r.arriveX, r.arriveZ, r.arriveY);
    }
}

// The warp on map `from` that leads toward map `target` in the fewest steps (most recently
// learned on a tie), or -1.
int WarpToward(u16 from, u16 target) {
    u16 maps[MAX_WARPS + 1];
    s16 firstHop[MAX_WARPS + 1];
    int n = 0, head = 0;
    maps[n] = from; firstHop[n] = -1; n++;
    for (int depth = 0; depth < WARP_DEPTH && head < n; depth++) {
        int end = n, best = -1;
        for (; head < end; head++) {
            for (int i = 0; i < g_warpCount; i++) {
                const Warp& w = g_warps[i];
                if (w.from != maps[head]) continue;
                int hop = firstHop[head] >= 0 ? firstHop[head] : i;
                if (w.to == target) {
                    if (best < 0 || g_warps[hop].used > g_warps[best].used) best = hop;
                    continue;
                }
                bool seen = false;
                for (int k = 0; k < n && !seen; k++) seen = maps[k] == w.to;
                if (!seen && n <= MAX_WARPS) { maps[n] = w.to; firstHop[n] = (s16)hop; n++; }
            }
        }
        if (best >= 0) return best;
    }
    return -1;
}

void DrawWarpLabels(const Screen& s, const View& v) {
    if (!v.me.valid || !v.me.inField) return;
    u64 now = osGetTime();
    // Friends grouped by the door that leads to them.
    int door[MAX_ID + 1];
    for (int id = 1; id <= MAX_ID; id++) {
        const Remote& r = v.remotes[id];
        door[id] = -1;
        if (id == v.myId || !r.active || !r.hasState || r.map == v.me.map || now - r.lastUpdate > STALE_MS) continue;
        if (g_notes[id].start && now - g_notes[id].start <= NOTE_MS) continue; // its leaving note is up
        door[id] = WarpToward(v.me.map, r.map);
    }
    bool done[MAX_ID + 1] = {};
    for (int id = 1; id <= MAX_ID; id++) {
        if (door[id] < 0 || done[id]) continue;
        const Warp& w = g_warps[door[id]];
        std::string names;
        int count = 0;
        bool inside = true;
        std::string here = PlaceOf(v.me.map), where;
        for (int k = id; k <= MAX_ID; k++) {
            if (door[k] != door[id]) continue;
            done[k] = true;
            if (count++) names += ", ";
            names += v.remotes[k].name;
            std::string place = PlaceOf(v.remotes[k].map);
            if (place != here) inside = false;
            if (where.empty()) where = place;
        }
        std::string text = names + (count > 1 ? " are " : " is ") + (inside ? "inside" : "in " + where);
        float fx, fy;
        if (!game::ProjectToTop(w.x, w.z + HEAD_HEIGHT, w.y, fx, fy)) continue;
        int lw = TextWidth(text) + 6;
        int x = (int)Clampf(fx - lw / 2.0f, 2, (float)(TOP_W - lw - 2)), y = (int)fy - 14;
        if (y < 18 || y > TOP_H - 14) continue; // off-screen: no arrow, the player list says where they are
        Rect(s, x, y, lw, 12, kBg);
        Text(s, text, x + 3, y + 1, kPlayerColors[id]);
    }
}

// ---- "/who" ----
// Lists who's online and where, as notices (asked for from the chat menu).
void ListWho(const View& v) {
    g_session.lock.Lock();
    bool asked = g_session.whoAsked;
    g_session.whoAsked = false;
    g_session.lock.Unlock();
    if (!asked) return;
    std::string lines[MAX_ID];
    u8 from[MAX_ID];
    int n = 0;
    for (int id = 1; id <= MAX_ID; id++) {
        const Remote& r = v.remotes[id];
        if (id == v.myId || !r.active) continue;
        std::string where = !r.hasState ? "starting the game"
                          : v.me.inField && r.map == v.me.map ? "here" : PlaceOf(r.map);
        if (r.hasState && r.away) where += " (busy)";
        else if (r.surfing) where += " (surfing)";
        else if (r.riding) where += " (bike)";
        lines[n] = std::string(r.name) + ": " + where;
        from[n++] = (u8)id;
    }
    g_session.lock.Lock();
    if (v.status != NetStatus::Connected) PushNotice("Not connected: " + v.statusText);
    else if (!n) PushNotice("Nobody else is online in " + v.room + ".");
    else {
        PushNotice(std::to_string(n + 1) + " online in " + v.room + ": you and " + std::to_string(n) +
                   (n == 1 ? " friend" : " friends"));
        for (int i = 0; i < n; i++) PushNotice(lines[i], from[i]);
    }
    g_session.lock.Unlock();
}

void DrawNotes(const Screen& s, const View& v) {
    u64 now = osGetTime();
    for (int id = 1; id <= MAX_ID; id++) {
        const Note& n = g_notes[id];
        if (!n.start || now - n.start > NOTE_MS || n.map != v.me.map) continue;
        float fx, fy;
        if (!game::ProjectToTop(n.x, n.z + HEAD_HEIGHT, n.y, fx, fy)) continue;
        float fade = 1.0f - (float)(now - n.start) / NOTE_MS;
        int rise = (int)((now - n.start) / 80); // drifts up as it fades
        int lw = TextWidth(n.text) + 6;
        int x = (int)Clampf(fx - lw / 2.0f, 2, (float)(TOP_W - lw - 2)), y = (int)fy - 14 - rise;
        const Color& c = kPlayerColors[id];
        Rect(s, x, y, lw, 12, Color(8, 14, 24, (u8)(170 * fade)));
        Text(s, n.text, x + 3, y + 1, Color(c.r, c.g, c.b, (u8)(255 * fade)));
    }
}

// ---- Notices ----
// Friend joined/left, chat, "X is talking to you": a small box styled like the game's text box at
// the top of the screen, one at a time, each for NOTICE_MS. Unlike the game's own box it never
// takes control from the player.
u64 g_noticeShown = 0;   // ms the front notice has been visible so far
u64 g_noticeLast = 0;

std::string ExpandNotice(const char* t) {
    std::string out;
    for (const char* p = t; *p; p++) {
        if (*p == 1 && p[1] && p[2] && p[3]) {
            u16 species = (u16)((p[1] & 0x7F) | (p[2] & 0x7F) << 7);
            out += MonName(species, (u8)p[3]);
            p += 3;
        } else if (*p == 2 && p[1] && p[2]) { // species name only
            u16 species = (u16)((p[1] & 0x7F) | (p[2] & 0x7F) << 7);
            char nm[16];
            if (!game::SpeciesName(species, nm, sizeof nm)) snprintf(nm, sizeof nm, "#%u", (unsigned)species);
            out += nm;
            p += 2;
        } else {
            out += *p;
        }
    }
    return out;
}

void DrawNotice(const Screen& s, const View& v, bool visible) {
    u64 now = osGetTime();
    u64 dt = g_noticeLast ? now - g_noticeLast : 0;
    g_noticeLast = now;
    if (!v.hasNotice) { g_noticeShown = 0; return; }
    if (!visible) return; // paused: shown in full once the game's UI is gone
    g_noticeShown += dt < 200 ? dt : 0;
    if (g_noticeShown >= NOTICE_MS) {
        g_session.lock.Lock();
        if (g_session.noticeCount > 0) {
            for (int i = 1; i < g_session.noticeCount; i++) g_session.notices[i - 1] = g_session.notices[i];
            g_session.noticeCount--;
        }
        g_session.lock.Unlock();
        g_noticeShown = 0;
        return;
    }
    std::string text = ExpandNotice(v.notice.text);
    int w = TextWidth(text) + 20;
    if (w > TOP_W - 8) { text.resize((TOP_W - 28) / 6); w = TextWidth(text) + 20; }
    // Slides down from the top edge, back up at the end.
    int slide = 0;
    if (g_noticeShown < 150) slide = (int)(150 - g_noticeShown) / 8;
    else if (g_noticeShown > NOTICE_MS - 150) slide = (int)(g_noticeShown - (NOTICE_MS - 150)) / 8;
    const int H = 20, X = (TOP_W - w) / 2, Y = 18 - slide;
    const Color border(84, 92, 110), panel(250, 250, 250), line(206, 214, 228), ink(56, 56, 64);
    Rect(s, X + 2, Y, w - 4, 2, border);
    Rect(s, X + 2, Y + H - 2, w - 4, 2, border);
    Rect(s, X, Y + 2, 2, H - 4, border);
    Rect(s, X + w - 2, Y + 2, 2, H - 4, border);
    Rect(s, X + 1, Y + 1, 1, 1, border);
    Rect(s, X + w - 2, Y + 1, 1, 1, border);
    Rect(s, X + 1, Y + H - 2, 1, 1, border);
    Rect(s, X + w - 2, Y + H - 2, 1, 1, border);
    Rect(s, X + 2, Y + 2, w - 4, H - 4, panel);
    Rect(s, X + 4, Y + 4, w - 8, H - 8, line, false);
    if (v.notice.from) Rect(s, X + 6, Y + 6, 3, 8, kPlayerColors[v.notice.from <= MAX_ID ? v.notice.from : 0]);
    Text(s, text, X + 12, Y + 5, ink);
}

// ---- Trades ----
// Both sides accepted: write their Pokemon into our offered slot, but only on the plain field
// (no menu, text box, script or battle), and only if that slot still holds what we offered.
void FinishTrade(const View& v) {
    g_session.lock.Lock();
    Trade& t = g_session.trade;
    if (!t.pendingWrite || !v.me.inField || v.me.suspended || v.me.locked || v.me.menuOpen ||
        game::GameScriptRunning()) {
        g_session.lock.Unlock();
        return;
    }
    u8 now[game::PK_SIZE];
    std::string them = g_session.remotes[t.partner].name;
    u8 partner = t.partner;
    bool same = game::ReadPartySlot(t.mySlot, now) && std::memcmp(now, t.mine, game::PK_SIZE) == 0;
    if (same && game::WritePartySlot(t.mySlot, t.theirs)) {
        u16 species = 0;
        u8 level = 0;
        game::DescribePokemon(t.theirs, species, level);
        PushNotice("Trade complete! You received " + MonName(species, level) + ".", partner);
    } else {
        PushNotice("Trade failed: your party changed.", partner);
    }
    t = Trade();
    g_session.lock.Unlock();
}

// ---- Watching a friend's battle ----
// While the game's text box shows what's out in a friend's battle, the bottom screen lists both
// sides with HP bars, updating live.
void DrawBattlePanel(const Screen& s, const Remote& r) {
    const Color ink(235, 235, 240), dim(150, 150, 160);
    Rect(s, 0, 0, BOT_W, BOT_H, Color(8, 14, 24, 235));
    Text(s, std::string(r.name) + "'s battle", 8, 4, kAccent);
    int y = 20;
    for (int side = 0; side < 2; side++) {
        Text(s, side == 0 ? std::string(r.name) + "'s team" : "Opponent", 8, y, dim);
        y += 12;
        for (int i = 0; i < r.battle.count[side] && y < BOT_H - 12; i++) {
            const game::BattleMon& m = r.battle.mon[side][i];
            std::string label = MonName(m.species, m.level);
            if (i == 0) label = "> " + label;
            Text(s, label, 12, y, m.hp ? ink : dim);
            constexpr int BX = 170, BW = 80;
            int fill = m.maxHp ? (int)(BW * (u32)m.hp / m.maxHp) : 0;
            float frac = m.maxHp ? (float)m.hp / m.maxHp : 0;
            Color bar = frac > 0.5f ? Color(70, 220, 110) : frac > 0.2f ? Color(240, 200, 60) : Color(235, 70, 60);
            Rect(s, BX - 1, y + 2, BW + 2, 7, Color(40, 44, 52));
            Rect(s, BX, y + 3, fill, 5, bar);
            char hp[16];
            snprintf(hp, sizeof hp, "%u/%u", (unsigned)m.hp, (unsigned)m.maxHp);
            Text(s, hp, BX + BW + 6, y, dim);
            y += 13;
        }
        y += 6;
    }
}

void DrawStatus(const Screen& s, const View& v) {
    bool ok = v.status == NetStatus::Connected;
    std::string line = ok ? ("oras-coop  " + v.room + "  #" + std::to_string(v.myId)) : v.statusText;
    int w = TextWidth(line) + 10;
    Rect(s, 2, 2, w, 13, kBg);
    Rect(s, 2, 2, 3, 13, ok ? kOk : kBad);
    Text(s, line, 8, 3, ok ? kOk : kBad);
}

void DrawMarker(const Screen& s, int id, const Remote& r, const View& v) {
    // Feet and head through the game's camera; the flat scale is only a fallback.
    float fx, fy, hx, hy;
    int sx, sy, labelY;
    if (game::ProjectToTop(r.dx, r.dz, r.dy, fx, fy) && game::ProjectToTop(r.dx, r.dz + HEAD_HEIGHT, r.dy, hx, hy)) {
        sx = (int)(fx + 0.5f);
        sy = (int)(fy + 0.5f);
        labelY = (int)(hy + 0.5f) - 14;
    } else {
        sx = PLAYER_SX + (int)((r.dx - v.me.x) * v.scale);
        sy = PLAYER_SY + (int)((r.dy - v.me.y) * v.scale);
        labelY = sy - 36;
    }
    const Color& col = kPlayerColors[id];
    std::string label = r.away ? std::string(r.name) + " (busy)" : std::string(r.name);

    bool onScreen = sx >= 8 && sx < TOP_W - 8 && sy >= 20 && sy < TOP_H - 8;
    if (onScreen) {
        if (r.surfing && !IsPuppeted(id)) {
            // Surfing without a trainer (mount failed / Real models off): ripples + a small marker.
            Rect(s, sx - 10, sy - 2, 20, 2, Color(220, 240, 255, 200));
            Rect(s, sx - 7, sy + 2, 14, 2, Color(170, 210, 255, 170));
            Rect(s, sx - 4, sy - 12, 8, 10, col);
            Rect(s, sx - 4, sy - 12, 8, 10, Color::Black, false);
        } else if (!IsPuppeted(id)) {
            // Ground shadow + body marker at the player's feet (a real NPC stands in otherwise).
            Rect(s, sx - 6, sy - 1, 12, 3, Color(0, 0, 0, 110));
            Rect(s, sx - 4, sy - 22, 8, 21, col);
            Rect(s, sx - 4, sy - 22, 8, 21, Color::Black, false);
        }
        int lw = TextWidth(label) + 6;
        Rect(s, sx - lw / 2, labelY, lw, 12, kBg);
        Text(s, label, sx - lw / 2 + 3, labelY + 1, col);
    } else {
        // Edge arrow pointing toward an off-screen player, with distance in tiles.
        int ex = (int)Clampf((float)sx, 6, TOP_W - 7);
        int ey = (int)Clampf((float)sy, 20, TOP_H - 7);
        float tiles = std::sqrt((r.dx - v.me.x) * (r.dx - v.me.x) + (r.dy - v.me.y) * (r.dy - v.me.y)) / TILE;
        char buf[40];
        snprintf(buf, sizeof buf, "%s%s %dt", r.name, r.away ? " (busy)" : "", (int)(tiles + 0.5f));
        std::string t = buf;
        int lw = TextWidth(t) + 6;
        int lx = (int)Clampf((float)(ex - lw / 2), 2, (float)(TOP_W - lw - 2));
        int ly = ey > TOP_H / 2 ? ey - 14 : ey + 6;
        Rect(s, ex - 3, ey - 3, 7, 7, col);
        Rect(s, lx, ly, lw, 12, kBg);
        Text(s, t, lx + 3, ly + 1, col);
    }
}

void DrawTop(const Screen& s) {
    g_view = Snapshot();
    const View& v = g_view;
    UpdateTalk(v);
    TrackDoors(v);
    TrackWarps(v);
    ListWho(v);
    UpdatePuppets(v.puppets, v.me, v.remotes, v.myId, g_talk.open ? g_talk.id : 0);
    FinishTrade(v);
    UpdateFriendFight(v);
    UpdateActivity(v.me, g_fight.armed || g_fight.running);
    // The X menu dims the field and the game's own text boxes sit where our labels go: step aside.
    // (The lock comes a few frames before the menu bit; once the menu is up the game stops
    // redrawing the top screen, so whatever we drew last would stay frozen on it.)
    bool gameUi = v.me.menuOpen || v.me.locked;
    if (!gameUi && v.me.inField) DrawStatus(s, v); // not over battles, the party screen, the bag...
    DrawNotice(s, v, !gameUi && v.me.inField);
    if (!v.overlay || gameUi) return;
    for (int id = 1; id <= MAX_ID; id++) {
        if (id != v.myId && Visible(v.remotes[id], v.me)) DrawMarker(s, id, v.remotes[id], v);
    }
    DrawNotes(s, v);
    DrawWarpLabels(s, v);
    if (g_talk.open && !g_talk.native) DrawTalkBox(s, g_talk, kPlayerColors[g_talk.id]);
}

void DrawBottom(const Screen& s) {
    const View& v = g_view; // filled by the top-screen pass this frame
    if (g_talk.open && g_talk.id >= 1 && g_talk.id <= MAX_ID && InBattle(v.remotes[g_talk.id])) {
        DrawBattlePanel(s, v.remotes[g_talk.id]);
        return;
    }
    // Off outside the field (title, battles) and while the game runs a script (dialogs, yes/no
    // prompts): our list and radar would cover its touch buttons.
    if (!v.overlay || !v.me.inField || v.me.suspended || v.me.menuOpen || v.me.locked) return;

    // Radar: 90x90 box in the top-right corner, north up, our player in the centre.
    constexpr int R = 45, CX = BOT_W - R - 4, CY = R + 4;
    Rect(s, CX - R, CY - R, 2 * R, 2 * R, kBg);
    Rect(s, CX - R, CY - R, 2 * R, 2 * R, kAccent, false);
    Rect(s, CX - 1, CY - 1, 3, 3, Color::White);

    // Player list on the left.
    constexpr int LIST_W = 212;
    int rows = 0;
    Rect(s, 4, 4, LIST_W, 12, kBg);
    Text(s, "Players", 8, 5, kAccent);
    for (int id = 1; id <= MAX_ID; id++) {
        const Remote& r = v.remotes[id];
        if (!r.active || id == v.myId) continue;
        bool same = Visible(r, v.me);
        bool sameMap = r.hasState && v.me.inField && r.map == v.me.map; // "here" even while we're busy
        char buf[64];
        const char* bike = r.surfing ? " (surfing)" : r.riding ? " (bike)" : "";
        const char* status = r.away ? " (busy)" : bike; // busy: battle, soaring...
        if (!r.hasState) snprintf(buf, sizeof buf, "%s  (starting the game)", r.name); // no position yet
        else if (sameMap) snprintf(buf, sizeof buf, "%s  here%s", r.name, status);
        else snprintf(buf, sizeof buf, "%s  %s%s", r.name, PlaceOf(r.map).c_str(), status);
        if (TextWidth(buf) > LIST_W - 8) buf[(LIST_W - 8) / 6] = 0;
        int y = 18 + rows * 12;
        Rect(s, 4, y, LIST_W, 12, kBg);
        Text(s, buf, 8, y + 1, sameMap ? kPlayerColors[id] : Color(160, 160, 160));
        rows++;

        if (same) {
            float rx = (r.dx - v.me.x) / RADAR_RANGE, ry = (r.dy - v.me.y) / RADAR_RANGE;
            int px = CX + (int)(Clampf(rx, -1, 1) * (R - 3));
            int py = CY + (int)(Clampf(ry, -1, 1) * (R - 3));
            Rect(s, px - 2, py - 2, 5, 5, kPlayerColors[id]);
        }
    }
    if (rows == 0) {
        Rect(s, 4, 18, LIST_W, 12, kBg);
        Text(s, "Nobody else here yet", 8, 19, Color(160, 160, 160));
    }
}

} // namespace

// Called by the game's talk event (Talk.cpp) when the player presses A at a friend's trainer.
bool FriendTalk(u16 charId, game::Dialog& out) {
    int id = (int)charId - 0xE0; // Puppet.cpp: character id = 0xE0 + player id
    if (id < 1 || id > MAX_ID) return false;
    out = FriendDialog(id);
    g_session.lock.Lock();
    std::string name = g_session.remotes[id].name;
    g_session.pendingTalk = (u8)id;
    g_session.lock.Unlock();
    g_talk = Talk();
    g_talk.open = true;
    g_talk.native = true;
    g_talk.id = id;
    g_talk.start = osGetTime();
    g_talk.name = name;
    g_talk.text = out.text;
    return true;
}

void FriendDialogResult(game::DialogResult what, int value, char* reply, int len) {
    OnDialogResult(what, value, reply, len);
}

bool OverlayCallback(const Screen& screen) {
    if (screen.IsTop) DrawTop(screen);
    else DrawBottom(screen);
    return true;
}

} // namespace coop
