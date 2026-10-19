// Shows text in the game's own field text box (the one NPCs talk in). The game runs all field
// dialogue as AMX (Pawn) scripts; we hand it a tiny script of our own:
//
//   TalkMdlMsg_Seq(MAGIC_LINE, ...)            open the text box (native 0x747308)
//   while (!CheckWinAllSuspend()) wait 1 frame wait until the text has printed
//   LastKeyWait()                              wait for A/B, like any NPC line
//   MsgWinCloseNo(0)                           close it
//
// How it's started (see ORAS-RE/NOTES.md, "Field scripts"):
//   FUN_003DC114(field, scriptId, 0...) is the game's "run script <id>" event. The id is turned
//   into a request by FUN_003FB594; we hook it so our id borrows the request of the Surf-check
//   script (ids 10050..10089: file 0x2E, its message file and native set) but loads our file id.
//   Our AMX image is put into the script manager's file container first (allocated on the
//   game heap: the game frees the slot's buffer itself once the script is loaded).
//   The box's text comes from GetString FUN_00139A34(msg, line, strbuf), which the native calls
//   through DllField's import stub at 0x006F32B8 (ldr pc,[pc,#-4]; .word target). We point that
//   stub at our function, which fills in our text for MAGIC_LINE (a line no message file has) and
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
constexpr u32 MAGIC_LINE = 0x7FF0;

// Native name hashes: h = c ^ (h * 0x83) over the name (FUN_00505B04).
constexpr u32 H_TALK_MSG = 0x9ADF1616;       // TalkMdlMsg_Seq
constexpr u32 H_WIN_READY = 0xC8E7FB61;      // CheckWinAllSuspend
constexpr u32 H_SUSPEND = 0x0B13A389;        // _Suspend (frames)
constexpr u32 H_KEY_WAIT = 0x358B0372;       // LastKeyWait
constexpr u32 H_CLOSE = 0xF01EEEE0;          // MsgWinCloseNo

// Opcodes of the game's Pawn VM (jump table *(0x0050468C), verifier FUN_001D3630).
constexpr u32 OP_PUSH_C = 0x27, OP_PROC = 0x2E, OP_RETN = 0x30, OP_JUMP = 0x33, OP_JZER = 0x35;
constexpr u32 OP_ZERO_PRI = 0x59, OP_ZERO_ALT = 0x5A, OP_HALT = 0x78, OP_SYSREQ_N = 0x87, OP_HALT_P = 0xD2;
constexpr u32 AMX_SLEEP = 12;                // HALT code: resume here next frame

constexpr u32 HDR = 0x3C, NATIVES = 5;
constexpr u32 STACK_HEAP = 0x800;

constexpr u32 MAX_CODE = 128;          // cells; the script is ~75
u32 s_image[0x100];
u32 s_imageSize = 0;     // bytes (= hea)
u32 s_stp = 0;

u16 s_text[96];
int s_textLen = 0;
volatile bool s_resolving = false;
Hook s_resolveHook;

void Build() {
    u32 code[MAX_CODE];
    u32 n = 0;
    auto op = [&](u32 v) { if (n < MAX_CODE) code[n] = v; n++; };
    auto sysreq = [&](u32 idx, u32 bytes) { op(OP_SYSREQ_N); op(idx); op(bytes); };
    op(OP_HALT_P);                     // 0x00: main returns here
    op(OP_PROC);                       // 0x04: main
    // TalkMdlMsg_Seq: the 19 arguments the Surf-check script passes (line, 0, -1, 0xB, 0, 0xAA,
    // ten zeros, -1, 0, 0), pushed last to first.
    const s32 args[19] = { (s32)MAGIC_LINE, 0, -1, 0xB, 0, 0xAA, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0 };
    for (int i = 18; i >= 0; i--) { op(OP_PUSH_C); op((u32)args[i]); }
    sysreq(0, 19 * 4);
    u32 loop = n * 4;
    sysreq(1, 0);                      // pri = CheckWinAllSuspend()
    u32 jz = n * 4; op(OP_JZER); op(0); // not ready yet -> wait a frame
    u32 jdone = n * 4; op(OP_JUMP); op(0);
    u32 wait = n * 4;
    op(OP_PUSH_C); op(1); sysreq(2, 4); // _Suspend(1) only arms the timer...
    op(OP_ZERO_PRI); op(OP_ZERO_ALT); op(OP_HALT); op(AMX_SLEEP); // ...HALT 12 yields the frame (as the game's scripts do)
    u32 jback = n * 4; op(OP_JUMP); op(0);
    u32 done = n * 4;
    sysreq(3, 0);                      // LastKeyWait()
    op(OP_PUSH_C); op(0); sysreq(4, 4); // MsgWinCloseNo(0)
    op(OP_ZERO_PRI);
    op(OP_RETN);
    s_imageSize = 0;
    if (n > MAX_CODE) return; // can't happen; ShowGameMessage refuses then
    // Jumps are relative to the jump opcode's own address.
    code[jz / 4 + 1] = wait - jz;
    code[jdone / 4 + 1] = done - jdone;
    code[jback / 4 + 1] = (u32)((s32)loop - (s32)jback);

    const u32 natives[NATIVES] = { H_TALK_MSG, H_WIN_READY, H_SUSPEND, H_KEY_WAIT, H_CLOSE };
    u32 nameTable = HDR + NATIVES * 8;     // u16 max name length (< 0x40 required)
    u32 cod = nameTable + 4;
    u32 dat = cod + n * 4;
    u32 hea = dat + 0x10;
    s_stp = hea + STACK_HEAP;
    if (hea > sizeof s_image) return;

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
    for (u32 i = 0; i < NATIVES; i++) { w32(HDR + i * 8, 0); w32(HDR + i * 8 + 4, natives[i]); }
    w16(nameTable, 0x1F);
    std::memcpy(b + cod, code, n * 4);
    s_imageSize = hea;
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
    if (line != MAGIC_LINE || !strbuf) return reinterpret_cast<Fn>(FN_GET_STRING)(msg, line, strbuf, x);
    // StrBuf: +4 u16 buffer, +8 u16 capacity, +0xA u16 length, +0xC u8 valid.
    volatile u8* valid = reinterpret_cast<volatile u8*>(strbuf + 0xC);
    u16* buf = *reinterpret_cast<u16**>(strbuf + 4);
    u16 cap = *reinterpret_cast<volatile u16*>(strbuf + 8);
    if (!*valid || !buf || cap < 2) return 0;
    int n = s_textLen < cap - 1 ? s_textLen : cap - 1;
    for (int i = 0; i < n; i++) buf[i] = s_text[i];
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

// Puts our script file (with `text`) into the script manager's container, ready to be run with
// OUR_ID. 1 = ready, 0 = the game is busy (a script file is loading), < 0 = can't here.
int PrepareMessage(const char* text, bool allowRunning) {
    if (!s_imageSize) return -1;
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

    // Text: ASCII -> UTF-16, broken into lines of at most 32 characters at spaces (0x0A = new line).
    s_textLen = 0;
    int lineStart = 0, lastSpace = -1;
    for (const char* p = text; *p && s_textLen < (int)(sizeof s_text / 2) - 1; p++) {
        u16 c = (u8)*p;
        if (c == '\n') { s_text[s_textLen++] = 0x0A; lineStart = s_textLen; lastSpace = -1; continue; }
        if (c == ' ') lastSpace = s_textLen;
        s_text[s_textLen++] = c;
        if (s_textLen - lineStart > 32 && lastSpace > lineStart) {
            s_text[lastSpace] = 0x0A;
            lineStart = lastSpace + 1;
        }
    }

    u32 e = entries + freeSlot * ENTRY_SIZE;
    u16 id = 0;
    Process::Read16(e + 8, id);
    if (id != OUR_FILE) {
        // Our script file, on the game heap (the game frees it once the script has been loaded).
        using Alloc = u32 (*)(u32, u32);
        u32 buf = reinterpret_cast<Alloc>(FN_ALLOC)(s_stp, heap);
        if (!buf) return -5;
        std::memcpy(reinterpret_cast<void*>(buf), s_image, s_imageSize);
        Process::Write32(e, buf);
        Process::Write32(e + 4, s_imageSize);
        Process::Write16(e + 8, OUR_FILE);
    }
    return 1;
}

constexpr u16 FRIEND_FIRST = 0xE1, FRIEND_LAST = 0xE8;  // ids of friends' trainers (Puppet.cpp)
constexpr u32 MM_VTABLE = 0x005DF560;
TalkTextFn s_textFn = nullptr;
Hook s_runHook;

// FUN_003DC114(field, id, target, ...): the game's "run script" event. Talking to an NPC runs the
// NPC's script id; our friends' trainers have none (0), and a request for script 0 never resolves:
// the script runner then spins forever (seen: game froze when A was pressed at a friend). So for
// them we run our message script with the friend's greeting instead; the game handles the rest
// of the conversation like any NPC's (player locked, A to close).
u32 RunScriptHook(u32 field, u32 id, u32 target, u32 p4, u32 p5, u32 p6, u32 p7) {
    HookContext& ctx = HookContext::GetCurrent();
    u32 vt = 0;
    u16 who = 0;
    if (id == 0 && target && Process::Read32(target, vt) && vt == MM_VTABLE && Process::Read16(target + 0xC, who) &&
        who >= FRIEND_FIRST && who <= FRIEND_LAST) {
        char text[64] = "...";
        if (s_textFn) s_textFn(who, text, sizeof text);
        if (PrepareMessage(text, true) == 1) {
            s_resolving = true;
            u32 ev = ctx.OriginalFunction<u32>(field, OUR_ID, target, p4, p5, p6, p7);
            s_resolving = false;
            return ev;
        }
    }
    return ctx.OriginalFunction<u32>(field, id, target, p4, p5, p6, p7);
}

} // namespace

bool InstallTalkHooks(TalkTextFn textFn) {
    Build();
    s_textFn = textFn;
    s_resolveHook.InitializeForMitm(FN_RESOLVE_ID, (u32)ResolveHook);
    s_runHook.InitializeForMitm(FN_RUN_SCRIPT, (u32)RunScriptHook);
    return s_resolveHook.Enable() == HookResult::Success && s_runHook.Enable() == HookResult::Success;
}

bool GameScriptRunning() {
    u32 field = 0, mgr = 0, cur = 0;
    return Read(FIELD_PTR, field) && Read(field + FIELD_SCRIPT_MGR, mgr) && Process::Read32(mgr + MGR_CURRENT, cur) && cur;
}

int ShowGameMessage(const char* text) {
    int r = PrepareMessage(text, false);
    if (r != 1) return r;
    u32 field = 0;
    if (!Read(FIELD_PTR, field)) return -2;
    using RunScript = u32 (*)(u32, u32, u32, u32, u32, u32, u32);
    s_resolving = true;
    u32 ev = reinterpret_cast<RunScript>(FN_RUN_SCRIPT)(field, OUR_ID, 0, 0, 0, 0, 0);
    s_resolving = false;
    return ev ? 1 : -6;
}

} // namespace game
