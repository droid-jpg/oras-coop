// Dialogs in the game's own field text box (the one NPCs talk in). The game runs all field
// dialogue as AMX (Pawn) scripts; we hand it a small script of our own, built per dialog:
//
//   [intro]     TalkMdlMsg_Seq(LINE_INTRO), wait until printed, A/B with the "more" arrow
//   text        TalkMdlMsg_Seq(LINE_TEXT), wait until printed
//   Message:    LastKeyWait(), close
//   YesNo:      YesNoWin_Seq(0,0,0,0) until answered; answer = GetWork(0x8010)
//               TalkMdlMsg_Seq(LINE_ANSWER + answer), wait, LastKeyWait(), close
//   YesNoPick:  as YesNo; on Yes: TalkMdlMsg_Seq(LINE_PICK), then the game's choice list
//               (ListMenuInit_Seq, ListMenuAdd(LINE_ITEM + i, i, 0) per item, ListMenuStart_Seq;
//               choice in 0x8010, 0xFFFF = B), TalkMdlMsg_Seq(LINE_SLOT + choice), wait, LastKeyWait()
//               (The game's party screen, CallPokeSelect, leaves the field faded out when called like
//               this: it needs the fade handling real scripts do around it.)
//
// The answers come back to us through the text: the box asks GetString for line LINE_ANSWER+n or
// LINE_SLOT+n, and our GetString passes n to the result callback, which acts on it and writes
// the line to show ("You offered ...").
//
// How it's started (see ORAS-RE/NOTES.md, "Field scripts"):
//   FUN_003DC114(field, scriptId, 0...) is the game's "run script <id>" event. The id is turned
//   into a request by FUN_003FB594; we hook it so our id borrows the request of the Surf-check
//   script (ids 10050..10089: file 0x2E, its message file and native set) but loads our file id.
//   Our AMX image is put into the script manager's file container first (allocated on the
//   game heap: the game frees the slot's buffer itself once the script is loaded).
//   The box's text comes from GetString FUN_00139A34(msg, line, strbuf), which the native calls
//   through DllField's import stub at 0x006F32B8 (ldr pc,[pc,#-4]; .word target). We point that
//   stub at our function, which fills in our text for our lines (lines no message file has) and
//   plain-calls the original for everything else. (A CTRPF MITM hook on GetString itself crashed:
//   worker threads call it too, and the hook context isn't there for them.)
#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cstring>
#include "Game.hpp"

using namespace CTRPluginFramework;

namespace game {

namespace {

constexpr u32 FIELD_PTR = 0x0062F7C4;        // -> field system (verified: +0x54 = script manager)
constexpr u32 SCRIPT_MGR_PTR = 0x005F46FC;   // -> script manager (same object)
constexpr u32 FIELD_SCRIPT_MGR = 0x54;
constexpr u32 MGR_FILE_HEAP = 0x04;          // heap the script files are loaded into
constexpr u32 MGR_CONTAINER = 0x24;          // loaded script files: u16 count, entries {code, size, u16 id}
constexpr u32 MGR_CURRENT = 0x34;            // running script request (0 = none)
constexpr u32 CONTAINER_ENTRIES = 0x04, CONTAINER_LOADER = 0x08, ENTRY_SIZE = 0x0C;
constexpr u16 NO_FILE = 0xFFFF;

constexpr u32 FN_RUN_SCRIPT = 0x003DC114;    // (field, id, target, p4, p5, p6, p7) -> event
constexpr u32 FN_RESOLVE_ID = 0x003FB594;    // (request+0x24, zone, id, x): id -> file, natives, text
constexpr u32 FN_GET_STRING = 0x00139A34;    // (msgData, line, strbuf, x) -> u16
constexpr u32 DLLFIELD_GET_STRING_STUB = 0x006F32B8; // DllField's import stub for it
constexpr u32 LDR_PC_PC_M4 = 0xE51FF004;      // ldr pc, [pc, #-4]
constexpr u32 FN_ALLOC = 0x001235A8;         // (size, heap) -> ptr
constexpr u32 FN_FILE_READY = 0x004C7224;    // (loader, entry) -> 0 while that file is loading

constexpr u32 OUR_ID = 32700;                // script id no range uses
constexpr u32 TEMPLATE_ID = 10050;           // Surf-check script: file 0x2E, message file 254, natives 0x643
constexpr u16 OUR_FILE = 0x3F0;              // container file id (the archive has ~0x56 files)
constexpr u32 REQ_FILE = 0x16;               // u16 file id in the resolved request

// Our text lines (no message file has this many lines).
constexpr u32 LINE_INTRO = 0x7FF0, LINE_TEXT = 0x7FF1, LINE_PICK = 0x7FF2;
constexpr u32 LINE_ITEM = 0x7FE0;                        // + item index
constexpr int MAX_ITEMS = DIALOG_ITEMS;
constexpr u32 LINE_ACTION_ITEM = 0x7FE8;                 // + Menu entry index
constexpr int MAX_ACTIONS = DIALOG_ACTIONS;
constexpr u32 LINE_ACTION = 0x7C00, LINE_ANSWER = 0x7D00, LINE_SLOT = 0x7E00; // + value & 0xFF

// Game work variables the natives answer in (the Surf script reads 0x8010 after YesNoWin_Seq).
// ListMenuStart_Seq stores the choice there too (literal 0x0074A4CC; B = 0xFFFF).
constexpr u32 WORK_ANSWER = 0x8010;

constexpr u32 BATTLE_CAN_LOSE = 4;           // _CallTrainerBattleCore flags: no whiteout on a loss

// Native name hashes: h = c ^ (h * 0x83) over the name (FUN_00505B04). GetWork's name isn't in
// DllField's table; the Surf script calls it with 0x8010 right after YesNoWin_Seq.
enum Native : u32 { N_TALK_MSG, N_WIN_READY, N_SUSPEND, N_KEY_WAIT, N_CLOSE, N_YESNO, N_GET_WORK,
                   N_MARK, N_AB_WAIT, N_UNMARK, N_LIST_INIT, N_LIST_ADD, N_LIST_START, N_TRAINER_BATTLE, NATIVES };
constexpr u32 NATIVE_HASH[NATIVES] = {
    0x9ADF1616, // TalkMdlMsg_Seq
    0xC8E7FB61, // CheckWinAllSuspend
    0x0B13A389, // _Suspend (frames)
    0x358B0372, // LastKeyWait
    0xF01EEEE0, // MsgWinCloseNo
    0x0511B06E, // YesNoWin_Seq: 0 until answered
    0x8E50F765, // GetWork(id)
    0xD04413AD, // SetKeyWaitMark: the "more" arrow
    0xC7DE8C7C, // _ABKeyWait
    0x4016ED32, // VanishKeyWaitMark
    0x75321BA3, // ListMenuInit_Seq(x, y, ?, B cancels, ?, ?) (DllField 0x748BB8)
    0x7903FD3E, // ListMenuAdd(line, value, other message file?) (0x744F34)
    0x5CB69D67, // ListMenuStart_Seq(): 0 until chosen (0x74A388)
    0x0B144EC1, // _CallTrainerBattleCore(0, trainer, 0, flags, 0, 0, 0): as trainer scripts (common
                // script file 81) call it; the script resumes once the battle is over (0x74E588)
};

// Opcodes of the game's Pawn VM (standard Pawn numbering; jump table *(0x0050468C), verifier
// FUN_001D3630). Jumps are relative to the jump opcode's own address.
constexpr u32 OP_CONST_ALT = 0x0C, OP_PUSH_PRI = 0x24, OP_PUSH_C = 0x27, OP_PROC = 0x2E, OP_RETN = 0x30;
constexpr u32 OP_JUMP = 0x33, OP_JZER = 0x35, OP_AND = 0x51, OP_ADD_C = 0x57;
constexpr u32 OP_EQ = 0x5F, OP_ZERO_PRI = 0x59, OP_ZERO_ALT = 0x5A, OP_HALT = 0x78, OP_SYSREQ_N = 0x87, OP_HALT_P = 0xD2;
constexpr u32 AMX_SLEEP = 12;                // HALT code: resume here next frame

constexpr u32 HDR = 0x3C;
constexpr u32 STACK_HEAP = 0x800;

// The Menu script (two choice lists, a battle and five messages of ~41 words each) is ~650 words.
constexpr u32 MAX_CODE = 1400;
u32 s_image[(0x200 + MAX_CODE * 4 + 0x10 + 0x800) / 4];
u32 s_imageSize = 0;     // bytes (= hea)
u32 s_stp = 0;

// Texts of the running dialog (UTF-16), and the reply the result callback wrote last.
struct Utf16 { u16 c[128]; int len = 0; };
Utf16 s_intro, s_text, s_pick, s_reply;
Utf16 s_items[MAX_ITEMS];
int s_itemCount = 0;
Utf16 s_actions[MAX_ACTIONS];
int s_actionCount = 0;
u32 s_replyLine = 0;     // line s_reply answers (GetString may ask for a line more than once)
DialogResultFn s_resultFn = nullptr;
volatile bool s_resolving = false;
Hook s_resolveHook;

// ASCII -> UTF-16, broken into lines of at most 32 characters at spaces (0x0A = new line).
void ToUtf16(const char* text, Utf16& out) {
    out.len = 0;
    int lineStart = 0, lastSpace = -1;
    const int max = (int)(sizeof out.c / 2) - 1;
    for (const char* p = text; *p && out.len < max; p++) {
        u16 c = (u8)*p;
        if (c == '\n') { out.c[out.len++] = 0x0A; lineStart = out.len; lastSpace = -1; continue; }
        if (c == ' ') lastSpace = out.len;
        out.c[out.len++] = c;
        if (out.len - lineStart > 32 && lastSpace > lineStart) {
            out.c[lastSpace] = 0x0A;
            lineStart = lastSpace + 1;
        }
    }
}

// A tiny assembler with forward jumps patched in place.
struct Asm {
    u32 code[MAX_CODE];
    u32 n = 0;
    void op(u32 v) { if (n < MAX_CODE) code[n] = v; n++; }
    u32 here() const { return n * 4; }
    u32 jump(u32 opcode) { u32 at = here(); op(opcode); op(0); return at; }   // returns the patch site
    void land(u32 site) { land(site, here()); }
    void land(u32 site, u32 target) { if (site / 4 + 1 < MAX_CODE) code[site / 4 + 1] = target - site; }
    void sysreq(Native idx, u32 args) { op(OP_SYSREQ_N); op(idx); op(args * 4); }
    // TalkMdlMsg_Seq: the 19 arguments the Surf-check script passes (line, 0, -1, 0xB, 0, 0xAA,
    // ten zeros, -1, 0, 0), pushed last to first; the line is a constant or pri.
    void message(bool linePri, u32 line = 0) {
        static const s32 args[19] = { 0, 0, -1, 0xB, 0, 0xAA, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0 };
        for (int i = 18; i >= 1; i--) { op(OP_PUSH_C); op((u32)args[i]); }
        if (linePri) op(OP_PUSH_PRI);
        else { op(OP_PUSH_C); op(line); }
        sysreq(N_TALK_MSG, 19);
    }
    // Yields one frame: _Suspend(1) only arms the timer, HALT 12 yields (as the game's scripts do).
    void waitFrame() {
        op(OP_PUSH_C); op(1); sysreq(N_SUSPEND, 1);
        op(OP_ZERO_PRI); op(OP_ZERO_ALT); op(OP_HALT); op(AMX_SLEEP);
    }
    // Calls `native` (with `args` already arranged by `push`) every frame until it returns non-zero.
    template <typename Push> void until(Native native, u32 args, Push push) {
        u32 loop = here();
        push();
        sysreq(native, args);
        u32 jz = jump(OP_JZER);
        u32 done = jump(OP_JUMP);
        land(jz);
        waitFrame();
        u32 back = jump(OP_JUMP);
        land(back, loop);
        land(done);
    }
    void waitPrinted() { until(N_WIN_READY, 0, [] {}); }
    void keyWait() { sysreq(N_KEY_WAIT, 0); }
    // Next page in the same window, as the game does it (closing and reopening at once stops the script).
    void pageWait() { sysreq(N_MARK, 0); sysreq(N_AB_WAIT, 0); sysreq(N_UNMARK, 0); }
    void close() { op(OP_PUSH_C); op(0); sysreq(N_CLOSE, 1); }
    void getWork(u32 id) { op(OP_PUSH_C); op(id); sysreq(N_GET_WORK, 1); }
    void priToLine(u32 base) { op(OP_CONST_ALT); op(0xFF); op(OP_AND); op(OP_ADD_C); op(base); }
};

// The game's choice list: ListMenuInit_Seq(x, y, ?, B cancels, ?, ?), one ListMenuAdd(line, value, 0)
// per entry, ListMenuStart_Seq; the choice ends up in work 0x8010 (B = 0xFFFF). y moves the list
// up (tuned live: 20 px per entry keeps it clear of the text box).
void ChoiceList(Asm& a, u32 firstLine, int count) {
    a.until(N_LIST_INIT, 6, [&] {
        const s32 init[6] = { 0, 0, 1, 0, 20 * count, 0 }; // pushed last to first
        for (s32 v : init) { a.op(OP_PUSH_C); a.op((u32)v); }
    });
    for (int i = 0; i < count; i++) {
        const u32 add[3] = { 0, (u32)i, firstLine + i }; // pushed last to first
        for (u32 v : add) { a.op(OP_PUSH_C); a.op(v); }
        a.sysreq(N_LIST_ADD, 3);
    }
    a.until(N_LIST_START, 0, [] {});
}

// Menu: the actions list; battle -> reply, close, battle; trade -> the party list; else the reply.
void BuildMenu(Asm& a, const Dialog& d) {
    ChoiceList(a, LINE_ACTION_ITEM, d.actionCount);
    u32 notBattle = 0, notTrade = 0;
    if (d.battleAction >= 0) {
        a.getWork(WORK_ANSWER);
        a.op(OP_CONST_ALT); a.op((u32)d.battleAction); a.op(OP_EQ);
        notBattle = a.jump(OP_JZER);
        a.getWork(WORK_ANSWER);
        a.priToLine(LINE_ACTION);
        a.message(true);
        a.waitPrinted();
        a.keyWait();
        a.close();
        // Flag 4 = a battle you may lose: the battle event (FUN_003DB1F0, state 5) whites the player
        // out after a loss unless event+0x2B is set, and only this flag sets it (FUN_003DA118).
        // Without it, losing to a friend ran the whiteout under our script and the game stopped.
        const u32 args[7] = { 0, 0, 0, BATTLE_CAN_LOSE, 0, d.battleTrainer, 0 }; // pushed last to first
        for (u32 v : args) { a.op(OP_PUSH_C); a.op(v); }
        a.sysreq(N_TRAINER_BATTLE, 7);
        a.op(OP_ZERO_PRI);
        a.op(OP_RETN);
        a.land(notBattle);
    }
    u32 toEnd = 0;
    if (d.tradeAction >= 0) {
        a.getWork(WORK_ANSWER);
        a.op(OP_CONST_ALT); a.op((u32)d.tradeAction); a.op(OP_EQ);
        notTrade = a.jump(OP_JZER);
        a.message(false, LINE_PICK);
        a.waitPrinted();
        ChoiceList(a, LINE_ITEM, d.itemCount);
        a.getWork(WORK_ANSWER);
        a.priToLine(LINE_SLOT);
        a.message(true);
        toEnd = a.jump(OP_JUMP);
        a.land(notTrade);
    }
    a.getWork(WORK_ANSWER);
    a.priToLine(LINE_ACTION);
    a.message(true);
    if (d.tradeAction >= 0) a.land(toEnd);
    a.waitPrinted();
}

bool Build(const Dialog& d) {
    s_imageSize = 0;
    Asm a;
    a.op(OP_HALT_P);                   // 0x00: main returns here
    a.op(OP_PROC);                     // 0x04: main
    if (d.intro[0]) {
        a.message(false, LINE_INTRO);
        a.waitPrinted();
        a.pageWait();
    }
    a.message(false, LINE_TEXT);
    a.waitPrinted();
    if (d.kind == DialogKind::Menu) {
        BuildMenu(a, d);
    } else if (d.kind != DialogKind::Message) {
        a.until(N_YESNO, 4, [&] { for (int i = 0; i < 4; i++) { a.op(OP_PUSH_C); a.op(0); } });
        a.getWork(WORK_ANSWER);
        u32 toAnswer = 0;
        if (d.kind == DialogKind::YesNoPick) {
            // Yes (non-zero, as the Surf script reads it): the choice list, then report the choice.
            toAnswer = a.jump(OP_JZER);
            a.message(false, LINE_PICK);
            a.waitPrinted();
            ChoiceList(a, LINE_ITEM, d.itemCount);
            a.getWork(WORK_ANSWER);
            a.priToLine(LINE_SLOT);
            a.message(true);
            u32 toEnd = a.jump(OP_JUMP);
            a.land(toAnswer);
            a.priToLine(LINE_ANSWER);  // No: report the answer
            a.message(true);
            a.land(toEnd);
        } else {
            a.priToLine(LINE_ANSWER);
            a.message(true);
        }
        a.waitPrinted();
    }
    a.keyWait();
    a.close();
    a.op(OP_ZERO_PRI);
    a.op(OP_RETN);
    if (a.n > MAX_CODE) return false;

    const u32 n = a.n;
    u32 nameTable = HDR + NATIVES * 8;     // u16 max name length (< 0x40 required)
    u32 cod = nameTable + 4;
    u32 dat = cod + n * 4;
    u32 hea = dat + 0x10;
    s_stp = hea + STACK_HEAP;
    if (hea > sizeof s_image) return false;

    u8* b = reinterpret_cast<u8*>(s_image);
    std::memset(b, 0, sizeof s_image);
    auto w32 = [&](u32 off, u32 v) { std::memcpy(b + off, &v, 4); };
    auto w16 = [&](u32 off, u16 v) { std::memcpy(b + off, &v, 2); };
    w32(0x00, hea);                 // size
    w16(0x04, 0xF1E0);              // magic
    b[0x06] = 10;                   // file version
    b[0x07] = 10;                   // VM version
    w16(0x08, 0x18);                // flags as the game's scripts, minus "compact"
    w16(0x0A, 8);                   // defsize
    w32(0x0C, cod);
    w32(0x10, dat);
    w32(0x14, hea);
    w32(0x18, s_stp);
    w32(0x1C, 4);                   // cip: main
    w32(0x20, HDR);                 // publics (none)
    w32(0x24, HDR);                 // natives
    w32(0x28, nameTable);           // libraries (none)
    w32(0x2C, nameTable);           // public variables (none)
    w32(0x30, nameTable);           // tags (none)
    w32(0x34, nameTable);           // name table
    w32(0x38, nameTable);           // overlays (none)
    for (u32 i = 0; i < NATIVES; i++) { w32(HDR + i * 8, 0); w32(HDR + i * 8 + 4, NATIVE_HASH[i]); }
    w16(nameTable, 0x1F);
    std::memcpy(b + cod, a.code, n * 4);
    s_imageSize = hea;
    return true;
}

void ResolveHook(u32 req, u32 zone, u32 id, u32 x) {
    if (id == OUR_ID && s_resolving) {
        HookContext::GetCurrent().OriginalFunction<void>(req, zone, TEMPLATE_ID, x);
        *reinterpret_cast<volatile u16*>(req + REQ_FILE) = OUR_FILE;
        return;
    }
    HookContext::GetCurrent().OriginalFunction<void>(req, zone, id, x);
}

u32 GetString(u32 msg, u32 line, u32 strbuf, u32 x) {
    using Fn = u32 (*)(u32, u32, u32, u32);
    const Utf16* t = nullptr;
    if (line == LINE_INTRO) t = &s_intro;
    else if (line == LINE_TEXT) t = &s_text;
    else if (line == LINE_PICK) t = &s_pick;
    else if (line >= LINE_ITEM && line < LINE_ITEM + MAX_ITEMS && (int)(line - LINE_ITEM) < s_itemCount) t = &s_items[line - LINE_ITEM];
    else if (line >= LINE_ACTION_ITEM && line < LINE_ACTION_ITEM + MAX_ACTIONS && (int)(line - LINE_ACTION_ITEM) < s_actionCount)
        t = &s_actions[line - LINE_ACTION_ITEM];
    else if (line >= LINE_ACTION && line < LINE_SLOT + 0x100) {
        if (line != s_replyLine) {
            // First time this answer is shown: act on it (once) and get the line to show.
            char reply[128] = "...";
            if (s_resultFn) {
                DialogResult what = line >= LINE_SLOT ? DialogResult::Slot
                                  : line >= LINE_ANSWER ? DialogResult::Answer : DialogResult::Action;
                u32 base = what == DialogResult::Slot ? LINE_SLOT : what == DialogResult::Answer ? LINE_ANSWER : LINE_ACTION;
                s_resultFn(what, (int)(line - base), reply, sizeof reply);
            }
            ToUtf16(reply, s_reply);
            s_replyLine = line;
        }
        t = &s_reply;
    }
    if (!t || !strbuf) return reinterpret_cast<Fn>(FN_GET_STRING)(msg, line, strbuf, x);
    // StrBuf: +4 u16 buffer, +8 u16 capacity, +0xA u16 length, +0xC u8 valid.
    volatile u8* valid = reinterpret_cast<volatile u8*>(strbuf + 0xC);
    u16* buf = *reinterpret_cast<u16**>(strbuf + 4);
    u16 cap = *reinterpret_cast<volatile u16*>(strbuf + 8);
    if (!*valid || !buf || cap < 2) return 0;
    int n = t->len < cap - 1 ? t->len : cap - 1;
    for (int i = 0; i < n; i++) buf[i] = t->c[i];
    buf[n] = 0;
    *reinterpret_cast<volatile u16*>(strbuf + 0xA) = (u16)n;
    return 0;
}

bool Read(u32 a, u32& v) { return Process::Read32(a, v) && v; }

// Points DllField's GetString stub at ours (again after DllField has been reloaded). False if
// DllField isn't where we expect it.
bool PatchTextStub() {
    u32 ins = 0, target = 0;
    if (!Process::Read32(DLLFIELD_GET_STRING_STUB, ins) || ins != LDR_PC_PC_M4 ||
        !Process::Read32(DLLFIELD_GET_STRING_STUB + 4, target))
        return false;
    if (target == (u32)GetString) return true;
    if (target != FN_GET_STRING) return false;
    return Process::Patch(DLLFIELD_GET_STRING_STUB + 4, (u32)GetString);
}

// Puts our script file for `d` into the script manager's container, ready to be run with
// OUR_ID. 1 = ready, 0 = the game is busy (a script file is loading), < 0 = can't here.
int PrepareDialog(const Dialog& d, bool allowRunning) {
    if (!FieldModuleLoaded()) return -2; // battle: DllField (and its text stub) isn't there
    u32 field = 0, mgr = 0, mgr2 = 0, container = 0, entries = 0, loader = 0, heap = 0, cur = 0;
    if (!Read(FIELD_PTR, field) || !Read(field + FIELD_SCRIPT_MGR, mgr) || !Read(SCRIPT_MGR_PTR, mgr2) || mgr != mgr2)
        return -2; // not on the field (or a layout we don't know)
    if (!allowRunning && (!Process::Read32(mgr + MGR_CURRENT, cur) || cur)) return 0; // a script runs: later
    if (!Read(mgr + MGR_CONTAINER, container) || !Read(container + CONTAINER_ENTRIES, entries) ||
        !Read(container + CONTAINER_LOADER, loader) || !Read(mgr + MGR_FILE_HEAP, heap))
        return -3;
    u16 count = 0;
    if (!Process::Read16(container, count) || count == 0 || count > 0x200) return -3;

    // The game refuses to start (and then crashes) while a script file is still loading.
    using FileReady = u32 (*)(u32, u32);
    int freeSlot = -1;
    for (int i = 0; i < count; i++) {
        u32 e = entries + i * ENTRY_SIZE;
        u16 id = 0;
        Process::Read16(e + 8, id);
        if (id == OUR_FILE) { freeSlot = i; continue; } // still there from a talk that didn't start: reuse
        if (id == NO_FILE) { if (freeSlot < 0) freeSlot = i; continue; }
        if (!reinterpret_cast<FileReady>(FN_FILE_READY)(loader, e)) return 0;
    }
    if (freeSlot < 0) return -4;
    if (!PatchTextStub()) return -7;
    if (!Build(d)) return -1;

    ToUtf16(d.intro, s_intro);
    ToUtf16(d.text, s_text);
    ToUtf16(d.pick, s_pick);
    s_itemCount = d.itemCount < MAX_ITEMS ? d.itemCount : MAX_ITEMS;
    for (int i = 0; i < s_itemCount; i++) ToUtf16(d.items[i], s_items[i]);
    s_actionCount = d.actionCount < MAX_ACTIONS ? d.actionCount : MAX_ACTIONS;
    for (int i = 0; i < s_actionCount; i++) ToUtf16(d.actions[i], s_actions[i]);
    s_replyLine = 0;

    // Our script file, on the game heap (the game frees it once the script has been loaded). A
    // leftover copy in the slot may be an older dialog: always write a fresh one.
    u32 e = entries + freeSlot * ENTRY_SIZE;
    using Alloc = u32 (*)(u32, u32);
    u32 buf = reinterpret_cast<Alloc>(FN_ALLOC)(s_stp, heap);
    if (!buf) return -5;
    std::memcpy(reinterpret_cast<void*>(buf), s_image, s_imageSize);
    Process::Write32(e, buf);
    Process::Write32(e + 4, s_imageSize);
    Process::Write16(e + 8, OUR_FILE);
    return 1;
}

constexpr u16 FRIEND_FIRST = 0xE1, FRIEND_LAST = 0xE8;  // ids of friends' trainers (Puppet.cpp)
constexpr u32 MM_VTABLE = 0x005DF560;
TalkDialogFn s_talkFn = nullptr;
Hook s_runHook;

// FUN_003DC114(field, id, target, ...): the game's "run script" event. Talking to an NPC runs the
// NPC's script id; our friends' trainers have none (0), and a request for script 0 never resolves:
// the script runner then spins forever (seen: game froze when A was pressed at a friend). So for
// them we run our dialog script instead; the game handles the rest of the conversation like any
// NPC's (player locked, A to close).
u32 RunScriptHook(u32 field, u32 id, u32 target, u32 p4, u32 p5, u32 p6, u32 p7) {
    HookContext& ctx = HookContext::GetCurrent();
    u32 vt = 0;
    u16 who = 0;
    if (id == 0 && target && Process::Read32(target, vt) && vt == MM_VTABLE && Process::Read16(target + 0xC, who) &&
        who >= FRIEND_FIRST && who <= FRIEND_LAST) {
        Dialog d;
        std::strcpy(d.text, "...");
        if (s_talkFn) s_talkFn(who, d);
        int r = PrepareDialog(d, true);
        if (r == -1) { // didn't fit: a plain line rather than the game's script id 0 (hangs)
            Dialog plain;
            std::strcpy(plain.text, "...");
            r = PrepareDialog(plain, true);
        }
        if (r == 1) {
            s_resolving = true;
            u32 ev = ctx.OriginalFunction<u32>(field, OUR_ID, target, p4, p5, p6, p7);
            s_resolving = false;
            return ev;
        }
    }
    return ctx.OriginalFunction<u32>(field, id, target, p4, p5, p6, p7);
}

} // namespace

bool InstallTalkHooks(TalkDialogFn talkFn, DialogResultFn resultFn) {
    s_talkFn = talkFn;
    s_resultFn = resultFn;
    s_resolveHook.InitializeForMitm(FN_RESOLVE_ID, (u32)ResolveHook);
    s_runHook.InitializeForMitm(FN_RUN_SCRIPT, (u32)RunScriptHook);
    return s_resolveHook.Enable() == HookResult::Success && s_runHook.Enable() == HookResult::Success;
}

bool GameScriptRunning() {
    u32 field = 0, mgr = 0, cur = 0;
    return Read(FIELD_PTR, field) && Read(field + FIELD_SCRIPT_MGR, mgr) && Process::Read32(mgr + MGR_CURRENT, cur) && cur;
}

int ShowGameDialog(const Dialog& d) {
    int r = PrepareDialog(d, false);
    if (r != 1) return r;
    u32 field = 0;
    if (!Read(FIELD_PTR, field)) return -2;
    using RunScript = u32 (*)(u32, u32, u32, u32, u32, u32, u32);
    s_resolving = true;
    u32 ev = reinterpret_cast<RunScript>(FN_RUN_SCRIPT)(field, OUR_ID, 0, 0, 0, 0, 0);
    s_resolving = false;
    return ev ? 1 : -6;
}

int ShowGameMessage(const char* text) {
    Dialog d;
    std::strncpy(d.text, text, sizeof d.text - 1);
    return ShowGameDialog(d);
}

} // namespace game
