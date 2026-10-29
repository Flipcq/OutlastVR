// ol_sound.cpp - [SOUND] (Test43) Outlast's Wwise sound engine, through the functions OLGame.exe
// exports by name (AK::SoundEngine::*, see `dumpbin /exports OLGame.exe`).
// =============================================================================
// [BREATH]    Miles's breathing. Every breath event the game posts on him (stress breath low / mid /
//             high, the running breath, danger / safe / pain breath, the breath after a hit, the peek
//             in / out) is recorded with its playing ID, so ol_interact's cover-your-mouth gesture can
//             hush them - Wwise Pause with a short fade - and let them out again (Resume). A breath the
//             game starts while the mouth is covered is paused as it starts. Letting go after holding
//             it posts one short breath of his own (the gasp).
// [AUDIODIAG] (the storm-loudness report) what the listener does - how many listeners, where, how
//             often - and the obstruction / occlusion and states the game sets, every 10 s.
// All hooks are on the exported entry points; the game posts from its game thread, the Wwise calls
// made here are queued by Wwise itself (thread safe by design).
// =============================================================================
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <cmath>
#include "MinHook.h"

extern "C" void OLProxyLog(const char* line);
static void SLog(const char* fmt, ...) {
    char b[400]; va_list a; va_start(a, fmt); vsprintf_s(b, fmt, a); va_end(a); OLProxyLog(b);
}

typedef uint32_t AkUniqueID; typedef uint32_t AkPlayingID; typedef uint64_t AkGameObjectID;
typedef void (*AkCallbackFunc)(int, void*);
typedef AkPlayingID (*PostEventId_t)(AkUniqueID, AkGameObjectID, uint32_t, AkCallbackFunc, void*, uint32_t, void*, AkPlayingID);
typedef AkPlayingID (*PostEventA_t)(const char*, AkGameObjectID, uint32_t, AkCallbackFunc, void*, uint32_t, void*, AkPlayingID);
typedef AkPlayingID (*PostEventW_t)(const wchar_t*, AkGameObjectID, uint32_t, AkCallbackFunc, void*, uint32_t, void*, AkPlayingID);
typedef int (*ExecAction_t)(AkUniqueID, int, AkGameObjectID, int32_t, int, AkPlayingID);
typedef AkUniqueID (*GetIdA_t)(const char*);
typedef AkUniqueID (*GetIdW_t)(const wchar_t*);
struct AkVec { float x, y, z; };
struct AkListenerPos { AkVec front, top, pos; };
typedef int (*SetListenerPos_t)(const AkListenerPos&, uint32_t);
typedef int (*SetActiveListeners_t)(AkGameObjectID, uint32_t);
typedef int (*SetObsOcc_t)(AkGameObjectID, uint32_t, float, float);
typedef int (*SetStateId_t)(uint32_t, uint32_t);
typedef int (*SetRtpcId_t)(uint32_t, float, AkGameObjectID, int32_t, int);

static PostEventId_t o_PostId = nullptr;  static PostEventA_t o_PostA = nullptr;  static PostEventW_t o_PostW = nullptr;
static ExecAction_t  g_exec = nullptr;    static GetIdA_t g_getIdA = nullptr;     static GetIdW_t g_getIdW = nullptr;
static SetListenerPos_t o_SetListener = nullptr; static SetActiveListeners_t o_SetActive = nullptr;
static SetObsOcc_t o_SetObsOcc = nullptr; static SetStateId_t o_SetState = nullptr; static SetRtpcId_t o_SetRtpc = nullptr;
static PostEventId_t g_post = nullptr;     // the real PostEvent(ID), for our own posts

enum { AK_Stop = 0, AK_Pause = 1, AK_Resume = 2 };
static const int kCurveLinear = 4;

// ---- [BREATH] --------------------------------------------------------------------------------------
// The breath events of WwiseDefaultBank_VO_Miles (CookedPCConsole\WwiseDefaultBank_VO_Miles.txt).
// heavy: the ones that mean he is out of breath or scared (the gesture is offered for those).
struct BreathEv { const char* name; bool heavy; bool stop; AkUniqueID id; };
static BreathEv s_ev[] = {
    { "VO_MILES_STRESS_BREATH", true, false }, { "VO_MILES_STRESS_BREATH_LOW", true, false },
    { "VO_MILES_STRESS_BREATH_MID", true, false }, { "VO_MILES_STRESS_BREATH_HIGH", true, false },
    { "VO_MILES_RUN_BREATH", true, false },
    { "VO_BRE_LOW_Long", true, false }, { "VO_BRE_LOW_Mid", true, false }, { "VO_BRE_LOW_Short", true, false },
    { "VO_BRE_MED_Long", true, false }, { "VO_BRE_MED_Mid", true, false }, { "VO_BRE_MED_Short", true, false },
    { "VO_BRE_HGH_Long", true, false }, { "VO_BRE_HGH_Mid", true, false }, { "VO_BRE_HGH_Short", true, false },
    { "VO_MILES_BRE_DAN", true, false }, { "VO_MILES_BRE_SAF", false, false }, { "VO_MILES_BRE_STH", true, false },
    { "VO_MILES_BRE_STH_OUT", false, false },
    { "VO_STRESS_BRE_PAIN_AI", true, false }, { "Play_AI_MILES_RUN_PANICKED_PAIN", true, false },
    { "Play_AI_MILES_RUN_NORMAL_PAIN", true, false }, { "Play_AI_MILES_WALK_IN_PAIN", true, false },
    { "VO_MILES_BREATH_HIT_INTENSITY", true, false }, { "VO_MILES_HIT_BRE_HIGH", true, false },
    { "VO_MILES_HIT_BRE_MED", true, false },
    { "VO_MILES_PEK_INSPI", false, false }, { "VO_MILES_PEK_EXPI", false, false },
    // the game's own "stop breathing like that" events
    { "VO_MILES_STRESS_BREATH_STOP", false, true }, { "VO_MILES_RUN_STOP", false, true },
    { "VO_MILES_BRE_DAN_Stop", false, true }, { "VO_STRESS_BRE_PAIN_AI_Stop", false, true },
    { "Stop_AI_MILES_WALK_IN_PAIN", false, true },
};
static const int kEvN = (int)(sizeof(s_ev) / sizeof(s_ev[0]));
static AkUniqueID s_gaspId = 0;
static bool s_idsOk = false;
struct Playing { AkUniqueID ev; AkPlayingID pid; AkGameObjectID obj; ULONGLONG ms; bool heavy; bool paused; };
static Playing s_play[24]; static int s_playN = 0;
static SRWLOCK s_lock = SRWLOCK_INIT;
static volatile ULONGLONG s_lastHeavyMs = 0;
static volatile long s_loopOn = 0;            // a stress / danger / pain breath loop is running (no stop yet)
static AkGameObjectID s_milesObj = 0;          // the object his breath plays on
static AkGameObjectID s_lastObj = 0;           // the last object any game event played on (door sounds fall back to it)
static volatile long s_hush = 0;
static volatile long s_breathPosts = 0, s_hushPaused = 0;
static int EvIndex(AkUniqueID id) { for (int i = 0; i < kEvN; ++i) if (s_ev[i].id == id) return i; return -1; }
// [AUDIODIAG] every distinct event the game posts (first 200), with its name when it was posted by name
static volatile long s_postsAll = 0;
static AkUniqueID s_seen[200]; static int s_seenN = 0; static SRWLOCK s_seenLock = SRWLOCK_INIT;
static void Seen(AkUniqueID id, const char* nm, const wchar_t* wn, AkGameObjectID obj) {
    InterlockedIncrement(&s_postsAll);
    AcquireSRWLockExclusive(&s_seenLock);
    bool known = false; for (int i = 0; i < s_seenN; ++i) if (s_seen[i] == id) { known = true; break; }
    if (!known && s_seenN < 200) s_seen[s_seenN++] = id;
    ReleaseSRWLockExclusive(&s_seenLock);
    if (known || s_seenN >= 200) return;
    char n8[96] = "";
    if (nm) strncpy_s(n8, nm, _TRUNCATE); else if (wn) { size_t c = 0; wcstombs_s(&c, n8, wn, _TRUNCATE); }
    SLog("[OLVR][AUDIODIAG] event posted: %u %s (object %llx)", id, n8[0] ? n8 : "(by id)", (unsigned long long)obj);
}

static void Track(AkUniqueID id, AkPlayingID pid, AkGameObjectID obj) {
    const int i = EvIndex(id);
    if (i < 0) return;
    const ULONGLONG now = GetTickCount64();
    InterlockedIncrement(&s_breathPosts);
    static ULONGLONG s_t0 = 0; if (!s_t0) s_t0 = now;
    SLog("[OLVR][BREATH] t=%.1f s: %s%s", (float)(now - s_t0) * 0.001f, s_ev[i].name, s_hush ? " (hushed as it starts)" : "");
    if (s_ev[i].stop) { InterlockedExchange(&s_loopOn, 0); return; }
    bool pauseNow = false;
    AcquireSRWLockExclusive(&s_lock);
    s_milesObj = obj;
    if (s_ev[i].heavy) { s_lastHeavyMs = now; if (strstr(s_ev[i].name, "STRESS") || strstr(s_ev[i].name, "DAN") || strstr(s_ev[i].name, "PAIN") || strstr(s_ev[i].name, "STH") || strstr(s_ev[i].name, "RUN_BREATH")) InterlockedExchange(&s_loopOn, 1); }
    // drop entries older than 60 s, keep the newest 24
    int w = 0;
    for (int k = 0; k < s_playN; ++k) if (now - s_play[k].ms < 60000) s_play[w++] = s_play[k];
    s_playN = w;
    if (s_playN == 24) { memmove(s_play, s_play + 1, sizeof(Playing) * 23); --s_playN; }
    if (pid) {
        Playing& p = s_play[s_playN++];
        p.ev = id; p.pid = pid; p.obj = obj; p.ms = now; p.heavy = s_ev[i].heavy; p.paused = false;
        if (s_hush) { p.paused = true; pauseNow = true; }
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (pauseNow && g_exec) { g_exec(id, AK_Pause, obj, 0, kCurveLinear, pid); InterlockedIncrement(&s_hushPaused); }
}
static AkPlayingID HookPostId(AkUniqueID id, AkGameObjectID obj, uint32_t fl, AkCallbackFunc cb, void* ck, uint32_t ne, void* ex, AkPlayingID pl) {
    const AkPlayingID r = o_PostId(id, obj, fl, cb, ck, ne, ex, pl);
    if (obj) s_lastObj = obj;
    Seen(id, nullptr, nullptr, obj);
    if (s_idsOk) Track(id, r, obj);
    return r;
}
static AkPlayingID HookPostA(const char* nm, AkGameObjectID obj, uint32_t fl, AkCallbackFunc cb, void* ck, uint32_t ne, void* ex, AkPlayingID pl) {
    const AkPlayingID r = o_PostA(nm, obj, fl, cb, ck, ne, ex, pl);
    if (obj) s_lastObj = obj;
    if (nm && g_getIdA) Seen(g_getIdA(nm), nm, nullptr, obj);
    if (s_idsOk && nm && g_getIdA) Track(g_getIdA(nm), r, obj);
    return r;
}
static AkPlayingID HookPostW(const wchar_t* nm, AkGameObjectID obj, uint32_t fl, AkCallbackFunc cb, void* ck, uint32_t ne, void* ex, AkPlayingID pl) {
    const AkPlayingID r = o_PostW(nm, obj, fl, cb, ck, ne, ex, pl);
    if (obj) s_lastObj = obj;
    if (nm && g_getIdW) Seen(g_getIdW(nm), nullptr, nm, obj);
    if (s_idsOk && nm && g_getIdW) Track(g_getIdW(nm), r, obj);
    return r;
}

// 1 = Miles is breathing hard right now (a heavy breath in the last 6.5 s - the pant after a run is ~4-6 s -
// or a stress loop running)
extern "C" int OLSound_BreathHeavy() {
    const ULONGLONG now = GetTickCount64();
    return (s_lastHeavyMs && now - s_lastHeavyMs < 6500) || s_loopOn ? 1 : 0;
}
extern "C" int OLSound_Ready() { return s_idsOk && g_exec ? 1 : 0; }
// Hush (1) or let out (0) every breath of his that is still about.
extern "C" void OLSound_Hush(int on) {
    if (!s_idsOk || !g_exec) return;
    on = on ? 1 : 0;
    if (InterlockedExchange(&s_hush, on) == on) return;
    Playing p[24]; int n = 0;
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&s_lock);
    for (int k = 0; k < s_playN; ++k) {
        if (now - s_play[k].ms > 45000) continue;
        if (on) { s_play[k].paused = true; p[n++] = s_play[k]; }
        else if (s_play[k].paused) { s_play[k].paused = false; p[n++] = s_play[k]; }
    }
    ReleaseSRWLockExclusive(&s_lock);
    for (int k = 0; k < n; ++k) g_exec(p[k].ev, on ? AK_Pause : AK_Resume, p[k].obj, on ? 220 : 350, kCurveLinear, p[k].pid);
    SLog("[OLVR][BREATH] %s - %d breath sound(s) %s", on ? "mouth covered: his breathing hushed" : "mouth uncovered: breathing again",
         n, on ? "paused" : "resumed");
}
// One short breath of his own (after holding it).
extern "C" void OLSound_Gasp() {
    if (!s_idsOk || !g_post || !s_gaspId || !s_milesObj) return;
    g_post(s_gaspId, s_milesObj, 0, nullptr, nullptr, 0, nullptr, 0);
    SLog("[OLVR][BREATH] the held breath let out (VO_BRE_HGH_Short on his voice)");
}

// [DOOR] the object a door sound should play on: the caller's, Miles's, or the last object the game
// posted any event on - so it is heard where he is even when the caller has no object of its own.
static AkGameObjectID SoundPortalObj(AkGameObjectID obj) {
    AkGameObjectID o = obj;
    if (!o) o = s_milesObj;
    if (!o) o = s_lastObj;
    if (!o) {
        AcquireSRWLockShared(&s_lock);
        for (int k = s_playN - 1; k >= 0; --k) if (s_play[k].obj) { o = s_play[k].obj; break; }
        ReleaseSRWLockShared(&s_lock);
    }
    return o;
}
extern "C" void OLSound_PostByName(const char* name, AkGameObjectID obj) {
    if (!s_idsOk || !g_post || !g_getIdA || !name) return;
    AkUniqueID id = g_getIdA(name);
    if (!id) return;
    const AkGameObjectID o = SoundPortalObj(obj);
    if (!o) { SLog("[OLVR][DOOR] no Wwise game object yet for door sound %s", name); return; }
    g_post(id, o, 0, nullptr, nullptr, 0, nullptr, 0);
    SLog("[OLVR][DOOR] posted door sound %s on obj %u", name, (unsigned)o);
}
// [DOOR] stop that event on the same object, with a short fade: the closing event ends with a harsh
// latch, so only its movement start is kept.
extern "C" void OLSound_StopByName(const char* name, AkGameObjectID obj) {
    if (!s_idsOk || !g_exec || !g_getIdA || !name) return;
    AkUniqueID id = g_getIdA(name);
    if (!id) return;
    const AkGameObjectID o = SoundPortalObj(obj);
    if (!o) return;
    g_exec(id, AK_Stop, o, 400, kCurveLinear, 0);   // fade out over 400 ms (the tail is cut early by the caller)
    SLog("[OLVR][DOOR] stopped door sound %s early on obj %u", name, (unsigned)o);
}
// [DOOR] set a Wwise RTPC (e.g. OpeningDoorIntensity) globally, so the next door event plays its
// slow / calm variant. Missing RTPC, export or object is a silent no-op.
extern "C" void OLSound_SetRTPCByName(const char* name, float v) {
    if (!s_idsOk || !o_SetRtpc || !g_getIdA || !name) return;
    const AkUniqueID id = g_getIdA(name);
    if (!id) return;
    o_SetRtpc(id, v, 0, 0, 0);   // 0 = global (AK_INVALID_GAME_OBJECT)
    SLog("[OLVR][DOOR] RTPC %s = %.2f (global)", name, v);
}

// ---- [AUDIODIAG] -----------------------------------------------------------------------------------
static volatile long s_lisCalls[4] = {}, s_activeCalls = 0, s_obsCalls = 0, s_stateCalls = 0, s_rtpcCalls = 0;
static AkListenerPos s_lisLast[4] = {};
static uint32_t s_activeMaskOr = 0;
static float s_occMax = 0.0f, s_occSum = 0.0f, s_obsMax = 0.0f; static long s_occN = 0;
static uint32_t s_stateLog[16][2]; static int s_stateLogN = 0;
static int HookSetListener(const AkListenerPos& p, uint32_t idx) {
    if (idx < 4) { InterlockedIncrement(&s_lisCalls[idx]); s_lisLast[idx] = p; }
    return o_SetListener(p, idx);
}
static int HookSetActive(AkGameObjectID o, uint32_t mask) {
    InterlockedIncrement(&s_activeCalls); s_activeMaskOr |= mask;
    return o_SetActive(o, mask);
}
static int HookSetObsOcc(AkGameObjectID o, uint32_t l, float obs, float occ) {
    InterlockedIncrement(&s_obsCalls);
    if (occ > s_occMax) s_occMax = occ; if (obs > s_obsMax) s_obsMax = obs;
    s_occSum += occ; ++s_occN;
    return o_SetObsOcc(o, l, obs, occ);
}
static int HookSetState(uint32_t g, uint32_t s) {
    InterlockedIncrement(&s_stateCalls);
    if (s_stateLogN < 16) { s_stateLog[s_stateLogN][0] = g; s_stateLog[s_stateLogN][1] = s; ++s_stateLogN; }
    return o_SetState(g, s);
}
extern "C" void OLSound_Report() {
    static ULONGLONG s_ms = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_ms < 10000) return;
    s_ms = now;
    long lc[4]; for (int i = 0; i < 4; ++i) lc[i] = InterlockedExchange(&s_lisCalls[i], 0);
    const AkListenerPos& a = s_lisLast[0];
    SLog("[OLVR][AUDIODIAG] listener updates /10s: #0 %ld #1 %ld #2 %ld #3 %ld | #0 at (%.0f %.0f %.0f) front (%.2f %.2f %.2f) | #1 at (%.0f %.0f %.0f)",
         lc[0], lc[1], lc[2], lc[3], a.pos.x, a.pos.y, a.pos.z, a.front.x, a.front.y, a.front.z,
         s_lisLast[1].pos.x, s_lisLast[1].pos.y, s_lisLast[1].pos.z);
    const long oc = InterlockedExchange(&s_obsCalls, 0);
    SLog("[OLVR][AUDIODIAG] events posted %ld | active-listener calls %ld (masks seen 0x%X) | obstruction/occlusion calls %ld: occlusion avg %.2f max %.2f, obstruction max %.2f | states set %ld, rtpc %ld | breath posts %ld (hush-paused %ld) heavy=%d",
         InterlockedExchange(&s_postsAll, 0),
         InterlockedExchange(&s_activeCalls, 0), s_activeMaskOr, oc, s_occN ? s_occSum / (float)s_occN : 0.0f, s_occMax, s_obsMax,
         InterlockedExchange(&s_stateCalls, 0), InterlockedExchange(&s_rtpcCalls, 0), InterlockedExchange(&s_breathPosts, 0), (long)s_hushPaused, OLSound_BreathHeavy());
    s_occMax = s_occSum = s_obsMax = 0.0f; s_occN = 0;
    for (int i = 0; i < s_stateLogN; ++i) SLog("[OLVR][AUDIODIAG]   state group %u -> %u", s_stateLog[i][0], s_stateLog[i][1]);
    s_stateLogN = 0;
}

static void* Exp(const char* n) { return (void*)GetProcAddress(GetModuleHandleW(nullptr), n); }
// Find an export whose name contains `sub` (used for Wwise setters whose mangled name varies between
// SDK versions). For SetRTPCValue we must pick the ID overload, not the name overload: a wrong
// signature would crash, so names taking a string (PEBD / PEB_W) are skipped.
static void* ExpContains(const char* sub, bool skipStringArgs) {
    HMODULE m = GetModuleHandleW(nullptr);
    if (!m) return nullptr;
    const unsigned char* base = (const unsigned char*)m;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const IMAGE_DATA_DIRECTORY& ex = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!ex.Size) return nullptr;
    const IMAGE_EXPORT_DIRECTORY* dir = (const IMAGE_EXPORT_DIRECTORY*)(base + ex.VirtualAddress);
    const DWORD* names = (const DWORD*)(base + dir->AddressOfNames);
    const WORD* ords = (const WORD*)(base + dir->AddressOfNameOrdinals);
    const DWORD* funcs = (const DWORD*)(base + dir->AddressOfFunctions);
    for (DWORD i = 0; i < dir->NumberOfNames; ++i) {
        const char* nm = (const char*)(base + names[i]);
        if (!strstr(nm, sub)) continue;
        if (skipStringArgs && (strstr(nm, "PEBD") || strstr(nm, "PEB_W"))) continue;
        return (void*)(base + funcs[ords[i]]);
    }
    return nullptr;
}
template <class T> static bool Hook(const char* name, void* fn, T* orig) {
    void* p = Exp(name);
    if (!p) return false;
    return MH_CreateHook(p, fn, (void**)orig) == MH_OK && MH_EnableHook(p) == MH_OK;
}
// Called from the engine hook setup in ol_ue3.cpp (MinHook is initialised by then).
extern "C" void OLSound_Install() {
    g_exec = (ExecAction_t)Exp("?ExecuteActionOnEvent@SoundEngine@AK@@YA?AW4AKRESULT@@KW4AkActionOnEventType@12@_KJW4AkCurveInterpolation@@K@Z");
    g_getIdA = (GetIdA_t)Exp("?GetIDFromString@SoundEngine@AK@@YAKPEBD@Z");
    g_getIdW = (GetIdW_t)Exp("?GetIDFromString@SoundEngine@AK@@YAKPEB_W@Z");
    g_post = (PostEventId_t)Exp("?PostEvent@SoundEngine@AK@@YAKK_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z");
    // SetRTPCValue by ID: only the numeric overload (string overloads are skipped by ExpContains).
    o_SetRtpc = (SetRtpcId_t)ExpContains("SetRTPCValue", true);
    if (!g_exec || !g_getIdA || !g_post) {
        SLog("[OLVR][SOUND] Wwise exports not found in this OLGame.exe (exec %p, id %p, post %p) - breath gesture off", g_exec, g_getIdA, g_post);
        return;
    }
    for (int i = 0; i < kEvN; ++i) s_ev[i].id = g_getIdA(s_ev[i].name);
    s_gaspId = g_getIdA("VO_BRE_HGH_Short");
    s_idsOk = true;
    const bool p1 = Hook("?PostEvent@SoundEngine@AK@@YAKK_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z", (void*)&HookPostId, &o_PostId);
    const bool p2 = Hook("?PostEvent@SoundEngine@AK@@YAKPEBD_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z", (void*)&HookPostA, &o_PostA);
    const bool p3 = Hook("?PostEvent@SoundEngine@AK@@YAKPEB_W_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z", (void*)&HookPostW, &o_PostW);
    if (p1 && o_PostId) g_post = o_PostId;   // our own posts skip the hook
    const bool d1 = Hook("?SetListenerPosition@SoundEngine@AK@@YA?AW4AKRESULT@@AEBUAkListenerPosition@@K@Z", (void*)&HookSetListener, &o_SetListener);
    const bool d2 = Hook("?SetActiveListeners@SoundEngine@AK@@YA?AW4AKRESULT@@_KK@Z", (void*)&HookSetActive, &o_SetActive);
    const bool d3 = Hook("?SetObjectObstructionAndOcclusion@SoundEngine@AK@@YA?AW4AKRESULT@@_KKMM@Z", (void*)&HookSetObsOcc, &o_SetObsOcc);
    const bool d4 = Hook("?SetState@SoundEngine@AK@@YA?AW4AKRESULT@@KK@Z", (void*)&HookSetState, &o_SetState);
    SLog("[OLVR][SOUND] Wwise hooked: PostEvent id/name/wide %d/%d/%d, listener %d, active %d, occlusion %d, state %d | %d breath events (stress breath id %u), gasp id %u",
         p1, p2, p3, d1, d2, d3, d4, kEvN, s_ev[0].id, s_gaspId);
}
