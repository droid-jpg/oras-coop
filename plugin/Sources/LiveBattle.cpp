// Live battles: both players pick their own moves. Each game runs the same battle (a friend battle,
// FriendBattle.cpp) with itself as client 0 and the friend as client 1, and the two games stay in
// step by trading what each player chose. See re/NOTES.md, "Live battles".
//
// The battle (DllBattle) is a server with one adapter per client. The server sends a command to
// every adapter (FUN_007682C8) and polls them all (FUN_00768150) until each client has answered;
// in a battle on one console the answer is a pointer (+0) and size (+4) the client leaves in its
// adapter, with the low nibble of +0x1783 set. Commands 3 (pick an action), 5 and 6 (pick a
// Pokemon to send in) answer with one u32 action per Pokemon out; the server copies them
// (FUN_00768018) and plays the turn. We replace the poll: our own answers (client 0) go to the
// friend, and client 1 (the game's AI, playing the friend's team) only counts as answered once the
// friend's answer for the same step has come in, which then replaces the AI's.
//
// Both games must then play the turn identically:
//   - Same random numbers: the battle seeds its RNG (TinyMT) with FUN_001122E4 (time based); while
//     armed that returns the seed both games agreed on.
//   - The two speed sorts (FUN_0075E010 actions, FUN_0075E1B8 Pokemon) break ties with a coin flip
//     in list order, and each game lists its own side first. We put the host's side first in both
//     games before sorting, so the same flip means the same thing.
//   - Things only the player's side gets in a one-console battle are turned off in both games
//     (FriendBattle.cpp): EXP, Pokemon-Amie affection, and the Shift-style "switch?" question
//     (forced to No here as well).
// Action words: low nibble = kind (1 move, 2 item, 3 switch...); a move's target position is bits
// 4..7, and positions alternate between the clients (0, 1 in a single battle), so the friend's
// targets have their low bit flipped.
//
// DllBattle shares one CRO slot with DllField and is loaded fresh for every battle, so its code is
// patched (a jump at each function's start) while a live battle is armed and DllBattle is in the
// slot, checked again every frame; the patches go away with the module. Game thread only, except
// the queues, which the network thread uses too.
#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cstdio>
#include <cstring>
#include "Game.hpp"

using namespace CTRPluginFramework;

namespace game {

namespace {

// DllBattle (loaded at 0x006F3000), with each function's first two instruction words.
struct Site { u32 addr, w0, w1; };
constexpr Site WAIT_ALL = { 0x00768150, 0xE92D4FF8, 0xE3A04000 };
constexpr Site SORT_ACTIONS = { 0x0075E010, 0xE92D47F0, 0xE2527000 };
constexpr Site SORT_POKES = { 0x0075E1B8, 0xE92D47F0, 0xE1A05000 };
constexpr u32 FN_SPEED = 0x00760414;          // (flow, battle Pokemon, 1) -> u16 speed for sorting
constexpr u32 LDR_PC_PC_M4 = 0xE51FF004;

// code.bin
constexpr u32 FN_SEED_FROM_TIME = 0x001122E4; // (tinymt state) -> seed: init from the clock
constexpr u32 FN_TINYMT_INIT = 0x0010FC58;    // (tinymt state, u32 seed)
constexpr u32 FN_RAND = 0x00175CEC;           // (tinymt state, n) -> [0, n)
constexpr u32 BTL_CALC = 0x08152AD8;          // battle calc globals: +0x54 -> the battle's RNG

// Server: per client 0xC bytes from +0x1C: adapter*, ..., u8 client id @+0x27 (0xFF = none).
constexpr u32 SV_CLIENTS = 0x1C, SV_CLIENT_SIZE = 0xC, SV_CLIENT_ID = 0x27;
// Adapter: +0 answer*, +4 answer size, +8 u16 command, +0x1781 client, +0x1782 state
// (1 sent, 3 waiting, 4 answered), +0x1783 low nibble = answered, high nibble = link battle.
constexpr u32 AD_ANSWER = 0, AD_SIZE = 4, AD_CMD = 8, AD_CLIENT = 0x1781, AD_STATE = 0x1782, AD_FLAGS = 0x1783;
constexpr u16 CMD_SELECT_ACTION = 3, CMD_ASK_SHIFT = 4, CMD_SELECT_COVER = 5, CMD_SELECT_CHANGE = 6;
// Battle Pokemon: u8 Pokemon id @0x19 (client 0: 0..5, client 1: 12..17, see FUN_0076D770).
constexpr u32 BP_ID = 0x19;

bool IsSelect(u16 cmd) { return cmd == CMD_SELECT_ACTION || cmd == CMD_SELECT_COVER || cmd == CMD_SELECT_CHANGE; }

u32 ClientOfId(u8 id) { return id < 6 ? 0 : id >= 12 && id < 18 ? 1 : id < 12 ? 2 : 3; }

// ---- State ----
LightLock s_lock;           // queues: game thread <-> network thread
bool s_lockReady = false;
constexpr int QUEUE = 8;
LiveAction s_out[QUEUE];    // our answers, waiting for the network thread
int s_outCount = 0;
LiveAction s_in[QUEUE];     // the friend's answers, by step
int s_inCount = 0;

volatile bool s_armed = false;
volatile bool s_partnerGone = false;
bool s_host = false;
u32 s_seed = 0;
bool s_seeded = false;
bool s_fellBack = false;    // the friend left: their AI plays on
bool s_desync = false;
u16 s_step0 = 0, s_step1 = 0;   // our answers sent / theirs used
bool s_fresh0 = false, s_fresh1 = false;
u16 s_cmd1 = 0;
u32 s_myCheck[64];          // our battle checksum at each step (mod 64), to compare with theirs
u32 s_answer1[6];           // their answer, as the server reads it
u32 s_no = 0;               // "No" to the Shift question
u32 s_lastOurs = 0, s_lastTheirs = 0;
u16 s_lastCmd = 0;
Hook s_seedHook;
bool s_patched = false;

void Lock() {
    if (!s_lockReady) { LightLock_Init(&s_lock); s_lockReady = true; }
    LightLock_Lock(&s_lock);
}
void Unlock() { LightLock_Unlock(&s_lock); }

u32 Rand(u32 n) {
    u32 state = *reinterpret_cast<volatile u32*>(BTL_CALC + 0x54);
    return reinterpret_cast<u32 (*)(u32, u32)>(FN_RAND)(state, n);
}

// HP of everything in the battle, the host's side first: the same number in both games.
u32 Checksum() {
    BattleState b;
    if (!ReadBattle(b)) return 0;
    u32 h = 2166136261u;
    for (int k = 0; k < 2; k++) {
        int side = s_host ? k : 1 - k;
        for (int i = 0; i < b.count[side]; i++) {
            const BattleMon& m = b.mon[side][i];
            u32 v = (u32)m.species << 16 | m.hp;
            for (int j = 0; j < 4; j++) h = (h ^ ((v >> (j * 8)) & 0xFF)) * 16777619u;
        }
    }
    return h;
}

// The friend's action, as our game sees it: their opponent's positions are ours and vice versa.
u32 Mirror(u32 w) {
    if ((w & 0xF) == 1) {
        u32 target = (w >> 4) & 0xF;
        if (target < 6) w = (w & ~0xF0u) | ((target ^ 1) << 4);
    }
    return w;
}

// ---- Replacements for DllBattle functions ----

// Client `client` answered command `cmd`: true once the server may use the answer.
bool Answered(u32 ad) {
    u8 flags = *reinterpret_cast<volatile u8*>(ad + AD_FLAGS);
    if (!(flags & 0xF)) return false;
    if (!s_armed) return true;
    u16 cmd = *reinterpret_cast<volatile u16*>(ad + AD_CMD);
    u8 client = *reinterpret_cast<volatile u8*>(ad + AD_CLIENT);
    if (client == 0) {
        if (cmd == CMD_ASK_SHIFT) { // only our game asks: the answer is always No
            s_no = 0;
            *reinterpret_cast<volatile u32*>(ad + AD_ANSWER) = (u32)&s_no;
            *reinterpret_cast<volatile u32*>(ad + AD_SIZE) = 4;
            return true;
        }
        if (IsSelect(cmd) && s_fresh0) {
            s_fresh0 = false;
            LiveAction a;
            a.step = s_step0++;
            a.cmd = (u8)cmd;
            u32 size = *reinterpret_cast<volatile u32*>(ad + AD_SIZE);
            const u32* words = *reinterpret_cast<u32* const*>(ad + AD_ANSWER);
            a.count = (u8)(words && size <= 24 ? size / 4 : 0);
            for (int i = 0; i < a.count; i++) a.words[i] = words[i];
            a.check = s_myCheck[a.step % 64];
            s_lastOurs = a.count ? a.words[0] : 0;
            s_lastCmd = cmd;
            if (!s_fellBack) {
                Lock();
                if (s_outCount < QUEUE) s_out[s_outCount++] = a;
                Unlock();
            }
        }
        return true;
    }
    if (client != 1 || !IsSelect(cmd) || !s_fresh1) return true;
    if (s_partnerGone) s_fellBack = true;
    if (s_fellBack) { s_fresh1 = false; return true; } // their AI's choice stands
    LiveAction a;
    bool have = false;
    Lock();
    for (int i = 0; i < s_inCount; i++) {
        if (s_in[i].step != s_step1) continue;
        a = s_in[i];
        s_in[i] = s_in[--s_inCount];
        have = true;
        break;
    }
    Unlock();
    if (!have) return false; // the friend is still choosing
    if (a.cmd != cmd || (a.check && s_myCheck[a.step % 64] && a.check != s_myCheck[a.step % 64])) s_desync = true;
    int n = a.count <= 6 ? a.count : 6;
    for (int i = 0; i < n; i++) s_answer1[i] = Mirror(a.words[i]);
    s_lastTheirs = n ? s_answer1[0] : 0;
    *reinterpret_cast<volatile u32*>(ad + AD_ANSWER) = n ? (u32)s_answer1 : 0;
    *reinterpret_cast<volatile u32*>(ad + AD_SIZE) = (u32)n * 4;
    s_step1++;
    s_fresh1 = false;
    return true;
}

// FUN_00768150(server): 1 once every client has answered the last command (one-console battles).
u32 WaitAll(u32 server) {
    u32 all = 1;
    for (u32 i = 0; i < 4; i++) {
        u32 e = server + SV_CLIENTS + i * SV_CLIENT_SIZE;
        if (*reinterpret_cast<volatile u8*>(e + SV_CLIENT_ID - SV_CLIENTS) == 0xFF) continue;
        u32 ad = *reinterpret_cast<volatile u32*>(e);
        volatile u8* st = reinterpret_cast<volatile u8*>(ad + AD_STATE);
        if (*st == 1) { // just sent
            *st = 3;
            u16 cmd = *reinterpret_cast<volatile u16*>(ad + AD_CMD);
            u8 client = *reinterpret_cast<volatile u8*>(ad + AD_CLIENT);
            if (s_armed && IsSelect(cmd)) {
                if (client == 0) { s_fresh0 = true; s_myCheck[s_step0 % 64] = Checksum(); }
                if (client == 1) { s_fresh1 = true; s_cmd1 = cmd; }
            }
        }
        if (*st == 3) {
            if (Answered(ad)) *st = 4;
            else all = 0;
        } else if (*st != 4) {
            all = 0;
        }
    }
    return all;
}

// Stable: the host's entries first, in both games.
template <typename ClientOf, typename Move>
void HostFirst(u32 count, ClientOf clientOf, Move move) {
    u32 hostClient = s_host ? 0 : 1;
    u32 keep = 0; // entries [0, keep) are the host's
    for (u32 i = 0; i < count; i++) {
        if (clientOf(i) != hostClient) continue;
        for (u32 j = i; j > keep; j--) move(j, j - 1); // bubble it down to `keep`
        keep++;
    }
}

// FUN_0075E010(flow, entries, count): the turn's actions by priority (entry +8), ties by coin flip.
// Entries are 16 bytes: battle Pokemon*, action, priority, u8 client.
void SortActions(u32 /*flow*/, u32 list, u32 count) {
    u32* e = reinterpret_cast<u32*>(list);
    auto swap = [&](u32 a, u32 b) { for (int k = 0; k < 4; k++) { u32 t = e[a * 4 + k]; e[a * 4 + k] = e[b * 4 + k]; e[b * 4 + k] = t; } };
    HostFirst(count, [&](u32 i) { return (u32)(e[i * 4 + 3] & 0xFF); }, swap);
    for (u32 i = 0; i < count; i++) {
        for (u32 j = i + 1; j < count; j++) {
            if (e[i * 4 + 2] <= e[j * 4 + 2] && (e[i * 4 + 2] != e[j * 4 + 2] || Rand(2) != 0)) swap(i, j);
        }
    }
}

// FUN_0075E1B8(list, flow): Pokemon by speed, ties by coin flip. List: battle Pokemon* [i] @0,
// u16 @0x28 [i], u16 speed @0x5A [i], u8 count @0x6E.
void SortPokes(u32 list, u32 flow) {
    u8 count = *reinterpret_cast<volatile u8*>(list + 0x6E);
    if (count < 2) return;
    u32* pp = reinterpret_cast<u32*>(list);
    u16* aux = reinterpret_cast<u16*>(list + 0x28);
    u16* speed = reinterpret_cast<u16*>(list + 0x5A);
    auto swap = [&](u32 a, u32 b) {
        u32 t = pp[a]; pp[a] = pp[b]; pp[b] = t;
        u16 x = aux[a]; aux[a] = aux[b]; aux[b] = x;
        u16 s = speed[a]; speed[a] = speed[b]; speed[b] = s;
    };
    HostFirst(count, [&](u32 i) { return ClientOfId(*reinterpret_cast<volatile u8*>(pp[i] + BP_ID)); }, swap);
    using Speed = u32 (*)(u32, u32, u32);
    for (u32 i = 0; i < count; i++) speed[i] = (u16)reinterpret_cast<Speed>(FN_SPEED)(flow, pp[i], 1);
    for (u32 i = 0; i < count; i++) {
        for (u32 j = i + 1; j < count; j++) {
            if (speed[i] < speed[j] || (speed[j] == speed[i] && Rand(2) != 0)) swap(i, j);
        }
    }
}

// ---- Patching DllBattle ----

// 1 = patched (now or before), 0 = not there yet (module still loading), -1 = unknown code.
int Ensure(const Site& s, u32 target) {
    u32 w0 = 0, w1 = 0;
    if (!Process::Read32(s.addr, w0) || !Process::Read32(s.addr + 4, w1)) return -1;
    if (w0 == LDR_PC_PC_M4 && w1 == target) return 1;
    if (w0 != s.w0 || w1 != s.w1) return 0;
    // The address first: until the jump is written, the function still starts with its own push.
    // (Game thread: no battle code runs while we write.)
    if (!Process::Patch(s.addr + 4, target) || !Process::Patch(s.addr, LDR_PC_PC_M4)) return -1;
    return 1;
}

u32 SeedHook(u32 state) {
    if (!s_armed) return HookContext::GetCurrent().OriginalFunction<u32>(state);
    reinterpret_cast<void (*)(u32, u32)>(FN_TINYMT_INIT)(state, s_seed);
    s_seeded = true;
    return s_seed;
}

} // namespace

bool InstallLiveBattleHook() {
    Lock();
    Unlock();
    s_seedHook.InitializeForMitm(FN_SEED_FROM_TIME, (u32)SeedHook);
    return s_seedHook.Enable() == HookResult::Success;
}

void ArmLiveBattle(bool host, u32 seed) {
    Lock();
    s_outCount = s_inCount = 0;
    Unlock();
    s_host = host;
    s_seed = seed ? seed : 0x4F524153;
    s_seeded = s_fellBack = s_desync = false;
    s_partnerGone = false;
    s_step0 = s_step1 = 0;
    s_fresh0 = s_fresh1 = false;
    std::memset(s_myCheck, 0, sizeof s_myCheck);
    s_lastOurs = s_lastTheirs = 0;
    s_lastCmd = 0;
    s_armed = true;
}

void DisarmLiveBattle() {
    s_armed = false;
    Lock();
    s_outCount = s_inCount = 0;
    Unlock();
}

bool LiveBattleArmed() { return s_armed; }

void UpdateLiveBattle() {
    if (!s_armed || !BattleModuleLoaded()) { s_patched = false; return; }
    int a = Ensure(WAIT_ALL, (u32)WaitAll);
    int b = Ensure(SORT_ACTIONS, (u32)SortActions);
    int c = Ensure(SORT_POKES, (u32)SortPokes);
    s_patched = a == 1 && b == 1 && c == 1;
}

void LivePartnerGone() { s_partnerGone = true; }

bool LivePopOutgoing(LiveAction& out) {
    Lock();
    bool have = s_outCount > 0;
    if (have) {
        out = s_out[0];
        for (int i = 1; i < s_outCount; i++) s_out[i - 1] = s_out[i];
        s_outCount--;
    }
    Unlock();
    return have;
}

void LivePushIncoming(const LiveAction& a) {
    Lock();
    if (s_inCount < QUEUE) s_in[s_inCount++] = a;
    Unlock();
}

LiveStatus GetLiveStatus() {
    LiveStatus s;
    s.armed = s_armed;
    s.patched = s_patched;
    s.seeded = s_seeded;
    s.fellBack = s_fellBack;
    s.desync = s_desync;
    s.stepOurs = s_step0;
    s.stepTheirs = s_step1;
    s.lastCmd = s_lastCmd;
    s.lastOurs = s_lastOurs;
    s.lastTheirs = s_lastTheirs;
    return s;
}

} // namespace game
