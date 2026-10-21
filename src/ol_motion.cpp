// ol_motion.cpp - [PHASE1] locomotion on the virtual pad.
// =============================================================================
// Everything here rewrites the fabricated XInput state the game polls (d3d9_proxy.cpp,
// Hook_XiGetState) and nothing else, so Outlast's own movement code still decides what is
// possible: collision, walk and sprint speed, scripted limits, whether there is room to stand.
// All of it acts only in ordinary gameplay (OLUE3_GameplayActive) and passes the pad through
// untouched everywhere else.
//
//  [SNAPTURN]   A flick of the right stick turns Miles a fixed angle. The game only turns at its
//               own stick rate, so the turn is driven closed-loop: full stick until the body yaw
//               the engine reports (before head injection) has moved the angle, easing off near
//               the end. The headset shows black meanwhile ([SNAPTURN] blink in ol_xr.cpp), so it
//               reads as a snap rather than a sweep.
//  [WALKDIR]    The left stick walks relative to Miles's body (classic), your head, or the way
//               your left controller points. Body stays the default: Outlast's chases are about
//               running one way while looking back.
//  [STICKPITCH] Optionally the right stick never tilts the camera; your head does.
//  [PHYSCROUCH] Duck for real and Miles crouches; stand and he stands. Edge-triggered when your
//               head crosses a depth below the recenter height, checked against the pawn's own
//               bIsCrouched (reflection, ol_ue3.cpp), retried while the game refuses (no room to
//               stand). Crouching with B still works and is never fought.
// =============================================================================
#include <Windows.h>
#include <math.h>
#include <cstdio>
#include <cstdarg>
#include "ol_xr.h"
#include "ol_motion.h"

extern "C" void OLProxyLog(const char* line);
extern "C" int  OLUE3_GameplayActive();
extern "C" int  OLUE3_GetBodyYaw(int* yawUnits);
extern "C" int  OLUE3_PawnCrouched();
extern "C" int  OLUE3_PawnNormalMove();
extern "C" int  OLUE3_PawnCrouchUnavailable();
extern "C" void OLUE3_SetCrouchLift(float m);
extern "C" int  OLXR_PosArmed();
extern "C" void OLXR_Blink(int ms);
extern "C" int  OLUE3_DirectTurnReady();          // (Test46) [DIRECTTURN]
extern "C" void OLUE3_TurnRate(float degPerSec);
extern "C" void OLUE3_TurnStep(float deg);
extern "C" void OLUE3_SetTurnScale(float s);       // (Test46d) [TURNSCALE]
extern "C" void OLUE3_TurnScaleTick();
// The game's own turn at full stick with its look speed at 300 (measured on the null HMD: 127 deg in 0.5 s,
// 64 deg in 0.25 s) and the snap speed asked of it.
static const float kGameTurnDegPerSec = 258.0f, kSnapDegPerSec = 720.0f;

static void MLog(const char* fmt, ...) {
    char b[320]; va_list a; va_start(a, fmt); vsprintf_s(b, fmt, a); va_end(a);
    OLProxyLog(b);
}

// ---- settings ([Input] in outlastvr.ini; missing keys = the previous behaviour) ----------------
static volatile long  g_turnMode    = 0;      // 0 smooth, 1 snap
static volatile long  g_snapDeg     = 30;
static volatile long  g_snapBlink   = 1;
static volatile float g_turnSpeed   = 1.0f;   // smooth turn: scales the stick, the game's rate is the ceiling
static volatile long  g_turnDegPerSec = 260;  // (Test46) smooth turn at full stick, degrees per second (260 = the game's fastest)
static volatile long  g_walkDir     = 0;      // 0 body, 1 head, 2 left hand
static volatile long  g_stickPitch  = 0;   // [STICKPITCH] Test37: off by default - the head does all up/down
static volatile long  g_physCrouch  = 0;
static volatile float g_crouchDepth = 0.30f;  // metres below the recenter height
static volatile long  g_crouchSeen  = -1;     // last game crouch state read, for the menu

extern "C" int   OLMotion_GetTurnMode() { return g_turnMode ? 1 : 0; }
extern "C" void  OLMotion_SetTurnMode(int v) { g_turnMode = v ? 1 : 0; }
extern "C" int   OLMotion_GetSnapDeg() { return (int)g_snapDeg; }
extern "C" void  OLMotion_SetSnapDeg(int v) { g_snapDeg = v < 15 ? 15 : (v > 90 ? 90 : v); }
extern "C" int   OLMotion_GetSnapBlink() { return g_snapBlink ? 1 : 0; }
extern "C" void  OLMotion_SetSnapBlink(int v) { g_snapBlink = v ? 1 : 0; }
extern "C" float OLMotion_GetTurnSpeed() { return g_turnSpeed; }
extern "C" int   OLMotion_GetTurnDegPerSec() { return (int)g_turnDegPerSec; }
extern "C" void  OLMotion_SetTurnDegPerSec(int v) { g_turnDegPerSec = v < 30 ? 30 : (v > 260 ? 260 : v); }
extern "C" void  OLMotion_SetTurnSpeed(float v) { g_turnSpeed = !(v >= 0.2f) ? 0.2f : (v > 1.0f ? 1.0f : v); }
extern "C" int   OLMotion_GetWalkDir() { return (int)g_walkDir; }
extern "C" void  OLMotion_SetWalkDir(int v) { g_walkDir = v < 0 ? 0 : (v > 2 ? 2 : v); }
extern "C" int   OLMotion_GetStickPitch() { return g_stickPitch ? 1 : 0; }
extern "C" void  OLMotion_SetStickPitch(int v) { g_stickPitch = v ? 1 : 0; }
extern "C" int   OLMotion_GetPhysCrouch() { return g_physCrouch ? 1 : 0; }
extern "C" void  OLMotion_SetPhysCrouch(int v) { g_physCrouch = v ? 1 : 0; }
extern "C" float OLMotion_GetCrouchDepth() { return g_crouchDepth; }
extern "C" void  OLMotion_SetCrouchDepth(float v) { g_crouchDepth = !(v >= 0.15f) ? 0.15f : (v > 0.60f ? 0.60f : v); }
extern "C" int   OLMotion_CrouchStatus() { return OLUE3_PawnCrouchUnavailable() ? -2 : (int)g_crouchSeen; }

static bool IniPath(wchar_t* out, size_t n) {
    wchar_t exe[MAX_PATH] = { 0 };
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    *(slash + 1) = 0;
    swprintf_s(out, n, L"%soutlastvr.ini", exe);
    return true;
}

void OLMotion_Init() {
    wchar_t ini[MAX_PATH];
    if (!IniPath(ini, MAX_PATH)) return;
    wchar_t v[32];
    OLMotion_SetTurnMode(GetPrivateProfileIntW(L"Input", L"TurnMode", 0, ini));
    OLMotion_SetSnapDeg(GetPrivateProfileIntW(L"Input", L"SnapTurnDeg", 30, ini));
    OLMotion_SetSnapBlink(GetPrivateProfileIntW(L"Input", L"SnapTurnBlink", 1, ini));
    GetPrivateProfileStringW(L"Input", L"SmoothTurnSpeed", L"1.0", v, 32, ini); OLMotion_SetTurnSpeed((float)_wtof(v));
    OLMotion_SetTurnDegPerSec(GetPrivateProfileIntW(L"Input", L"SmoothTurnDegPerSec", 260, ini));
    OLMotion_SetWalkDir(GetPrivateProfileIntW(L"Input", L"WalkDirection", 0, ini));
    OLMotion_SetStickPitch(GetPrivateProfileIntW(L"Input", L"StickPitch", 0, ini));
    OLMotion_SetPhysCrouch(GetPrivateProfileIntW(L"Input", L"PhysicalCrouch", 0, ini));
    GetPrivateProfileStringW(L"Input", L"CrouchDepthM", L"0.30", v, 32, ini); OLMotion_SetCrouchDepth((float)_wtof(v));
    MLog("[OLVR][PHASE1] turn=%s snap=%ld deg blink=%ld smoothSpeed=%ld deg/s walk=%s stickPitch=%ld physCrouch=%ld depth=%.2fm",
         g_turnMode ? "snap" : "smooth", g_snapDeg, g_snapBlink, g_turnDegPerSec,
         g_walkDir == 1 ? "head" : (g_walkDir == 2 ? "left hand" : "body"),
         g_stickPitch, g_physCrouch, g_crouchDepth);
}

void OLMotion_Save(const wchar_t* ini) {
    wchar_t b[32];
    swprintf_s(b, L"%ld", g_turnMode);   WritePrivateProfileStringW(L"Input", L"TurnMode", b, ini);
    swprintf_s(b, L"%ld", g_snapDeg);    WritePrivateProfileStringW(L"Input", L"SnapTurnDeg", b, ini);
    swprintf_s(b, L"%ld", g_snapBlink);  WritePrivateProfileStringW(L"Input", L"SnapTurnBlink", b, ini);
    swprintf_s(b, L"%.3f", g_turnSpeed); WritePrivateProfileStringW(L"Input", L"SmoothTurnSpeed", b, ini);
    swprintf_s(b, L"%ld", g_turnDegPerSec); WritePrivateProfileStringW(L"Input", L"SmoothTurnDegPerSec", b, ini);
    swprintf_s(b, L"%ld", g_walkDir);    WritePrivateProfileStringW(L"Input", L"WalkDirection", b, ini);
    swprintf_s(b, L"%ld", g_stickPitch); WritePrivateProfileStringW(L"Input", L"StickPitch", b, ini);
    swprintf_s(b, L"%ld", g_physCrouch); WritePrivateProfileStringW(L"Input", L"PhysicalCrouch", b, ini);
    swprintf_s(b, L"%.3f", g_crouchDepth); WritePrivateProfileStringW(L"Input", L"CrouchDepthM", b, ini);
}

// ---- [WALKDIR] -------------------------------------------------------------------------------
// Yaw in the recenter frame, + = right, the same convention as g_olYawRad (ol_xr.cpp extracts it
// from the forward vector as atan2(fx, -fz)). For a controller the grip's -Z, flattened; a
// controller pointing nearly straight up or down has no usable heading and returns false.
static bool HandYaw(const OLXrPose& q, float* yaw) {
    const float fx = -2.0f * (q.qx * q.qz + q.qw * q.qy);
    const float fz = -(1.0f - 2.0f * (q.qx * q.qx + q.qy * q.qy));
    if (fx * fx + fz * fz < 0.09f) return false;
    *yaw = atan2f(fx, -fz);
    return true;
}

static void WalkDirection(const OLXrPad& p, float* mx, float* my) {
    if (g_walkDir == 0 || (*mx == 0.0f && *my == 0.0f)) return;
    float a = 0.0f;
    if (g_walkDir == 1) {
        if (!g_olPoseValid) return;
        a = g_olYawRad;
    } else {
        if (!(p.handValid & 1u) || !HandYaw(p.handPose[0], &a)) return;
    }
    // Stick (x right, y forward) in the chosen frame -> Miles's body frame.
    const float c = cosf(a), s = sinf(a), x = *mx, y = *my;
    *my = y * c - x * s;
    *mx = y * s + x * c;
}

// ---- [SNAPTURN] ------------------------------------------------------------------------------
static int   s_snapDir = 0, s_snapRemain = 0, s_snapLastYaw = 0, s_snapTarget = 0;
static ULONGLONG s_snapStart = 0;
static bool  s_snapArmed = true;

static void SnapReset() { s_snapDir = 0; }

static void SnapTurn(float* lx, ULONGLONG now) {
    const float sx = *lx;
    if (fabsf(sx) < 0.35f) s_snapArmed = true;
    int yaw = 0;
    const bool yawOk = OLUE3_GetBodyYaw(&yaw) != 0;
    if (s_snapDir == 0) {
        *lx = 0.0f;   // in snap mode the stick never turns smoothly
        if (!s_snapArmed || fabsf(sx) < 0.75f || !yawOk) return;
        s_snapDir = sx > 0.0f ? 1 : -1;
        s_snapTarget = s_snapRemain = (int)((float)g_snapDeg * 65536.0f / 360.0f);
        s_snapLastYaw = yaw; s_snapStart = now; s_snapArmed = false;
    }
    if (yawOk) {
        int d = (yaw - s_snapLastYaw) & 0xFFFF;
        if (d > 32768) d -= 65536;
        s_snapRemain -= d * s_snapDir;
        s_snapLastYaw = yaw;
    }
    const ULONGLONG limit = 400u + (ULONGLONG)g_snapDeg * 20u;   // 30 deg -> 1.0 s, 90 deg -> 2.2 s
    if (s_snapRemain <= 90 || (now - s_snapStart) > limit || !yawOk) {
        static long s_logged = 0;
        if (s_logged < 6) {
            ++s_logged;
            MLog("[OLVR][SNAPTURN] %s %.1f of %ld deg in %llu ms%s", s_snapDir > 0 ? "right" : "left",
                 (s_snapTarget - s_snapRemain) * 360.0f / 65536.0f, g_snapDeg, now - s_snapStart,
                 (now - s_snapStart) > limit ? " (time limit)" : "");
        }
        s_snapDir = 0;
        *lx = 0.0f;
        return;
    }
    // Full stick until the last 15 degrees, then ease off so the game's own turn rate cannot
    // carry it far past the target. 0.6 keeps well clear of UE3's right-stick dead zone (~0.27).
    float out = (float)s_snapRemain / 2730.0f;
    if (out > 1.0f) out = 1.0f;
    if (out < 0.6f) out = 0.6f;
    *lx = (float)s_snapDir * out;
    // Dark for the turn, but never longer than 0.35 s: a slow game turn rate finishes in view.
    if (g_snapBlink && now - s_snapStart < 350u) OLXR_Blink(70);
}

// (Test46) [DIRECTTURN] snap: one flick, the whole angle in one frame (the blink covers it).
static bool s_snapArmedD = true;
static void SnapTurnDirect(float* lx) {
    const float sx = *lx;
    *lx = 0.0f;
    if (fabsf(sx) < 0.35f) { s_snapArmedD = true; return; }
    if (!s_snapArmedD || fabsf(sx) < 0.75f) return;
    s_snapArmedD = false;
    OLUE3_TurnStep(sx > 0.0f ? (float)g_snapDeg : -(float)g_snapDeg);
    if (g_snapBlink) OLXR_Blink(70);
}

// ---- [PHYSCROUCH] ----------------------------------------------------------------------------
static bool  s_wantCrouch = false;    // the head is below the threshold (with hysteresis)
static int   s_crouchGoal = -1;       // game state being reached for: -1 none, 0 stand, 1 crouch
static int   s_crouchTries = 0;
static ULONGLONG s_crouchNext = 0, s_crouchPulseUntil = 0;
static bool  s_crouchEngaged = false; // the game is crouched BECAUSE the player ducked
static float s_crouchLift = 0.0f;
static ULONGLONG s_crouchLiftMs = 0;

static WORD PhysicalCrouch(bool play, ULONGLONG now) {
    int game = -1;
    // Only in ordinary locomotion: ducking inside a locker, under a bed or on a ledge must never
    // press the game's crouch.
    // PawnCrouched first: it is what resolves the reflection that PawnNormalMove reads.
    if (play && g_physCrouch && (game = OLUE3_PawnCrouched()) >= 0 && OLUE3_PawnNormalMove() == 1) {
        OLXrFrameHands fh;
        if (game >= 0 && OLXR_PosArmed() && OLXR_GetFrameHands(&fh)) {
            const float y = fh.headRaw[1];
            const float depth = g_crouchDepth;
            const bool want = s_wantCrouch ? (y < -(depth - 0.10f)) : (y < -depth);
            if (want != s_wantCrouch) {
                s_wantCrouch = want;
                s_crouchGoal = want ? 1 : 0; s_crouchTries = 0; s_crouchNext = now;
                MLog("[OLVR][PHYSCROUCH] head %.2f m below the recenter height -> %s (game %s)",
                     -y, want ? "crouch" : "stand", game ? "crouched" : "standing");
            }
        }
        if (s_crouchGoal >= 0 && game >= 0) {
            if (game == s_crouchGoal) {
                s_crouchEngaged = (s_crouchGoal == 1);
                s_crouchGoal = -1;
            } else if (now >= s_crouchNext) {
                // The game's crouch is a toggle (B). Press it ONCE per change of intent: if there
                // is no room to stand, the game may hold the press as a wish and stand when it
                // can, and a second press would cancel that. After 2.5 s with no change it is
                // left to the player (and to B).
                if (s_crouchTries < 1) {
                    s_crouchPulseUntil = now + 120; s_crouchNext = now + 2500; ++s_crouchTries;
                } else {
                    MLog("[OLVR][PHYSCROUCH] the game has not %s yet - leaving it to the game and to B",
                         s_crouchGoal ? "crouched" : "stood up");
                    s_crouchGoal = -1;
                }
            }
        }
        if (s_crouchEngaged && game == 0) s_crouchEngaged = false;   // stood up some other way
    } else {
        s_crouchGoal = -1;
        s_wantCrouch = false;
        if (!play || !g_physCrouch) s_crouchEngaged = false;
    }
    g_crouchSeen = game;

    // Hand back the part of the real duck the game's own crouch already took, ramped so the view
    // never steps. ol_ue3 adds it to the head height for the camera and the arms alike.
    const float target = s_crouchEngaged ? g_crouchDepth : 0.0f;
    const float dt = s_crouchLiftMs ? (float)(now - s_crouchLiftMs) * 0.001f : 0.0f;   // 64-bit ms
    s_crouchLiftMs = now;
    const float step = 1.5f * (dt > 0.1f ? 0.1f : dt);
    if (s_crouchLift < target) s_crouchLift = (s_crouchLift + step < target) ? s_crouchLift + step : target;
    else if (s_crouchLift > target) s_crouchLift = (s_crouchLift - step > target) ? s_crouchLift - step : target;
    OLUE3_SetCrouchLift(s_crouchLift);

    return (s_crouchPulseUntil && now < s_crouchPulseUntil) ? (WORD)0x2000 : (WORD)0;   // B = OLA_CrouchToggle
}

// [STICKPITCH] (Test37) With the stick no longer tilting the view, whatever tilt the game itself
// still has (a cutscene that ended looking down, old stick input) would stay forever and leave
// the horizon off. Steer it back to level through the game's own look input, gently, then stop.
// The sign is checked against the result: if a nudge makes the tilt worse, the direction flips.
extern "C" int OLUE3_GetBasePitch(int* pitchUnits);
static float LevelPitch(ULONGLONG now) {
    static float s_sign = -1.0f;            // stick down for a positive (upward) tilt
    static float s_lastAbs = -1.0f;
    static ULONGLONG s_lastCheck = 0;
    int u = 0;
    if (!OLUE3_GetBasePitch(&u)) return 0.0f;
    const float deg = (float)u * (360.0f / 65536.0f);
    const float a = fabsf(deg);
    if (a < 1.5f) { s_lastAbs = -1.0f; return 0.0f; }
    if (now - s_lastCheck > 250) {
        if (s_lastAbs >= 0.0f && a > s_lastAbs + 0.5f) {
            s_sign = -s_sign;
            MLog("[OLVR][STICKPITCH] leveling made the tilt worse - flipping direction (tilt %.1f deg)", deg);
        }
        s_lastAbs = a; s_lastCheck = now;
    }
    const float mag = 0.32f + (a > 20.0f ? 0.30f : a * 0.015f);   // just past UE3's stick dead zone
    return (deg > 0.0f ? s_sign : -s_sign) * mag;
}

WORD OLMotion_Process(const OLXrPad& p, bool camcorderUp, float* moveX, float* moveY,
                      float* lookX, float* lookY) {
    const ULONGLONG now = GetTickCount64();
    const bool play = OLUE3_GameplayActive() != 0;
    if (play) {
        WalkDirection(p, moveX, moveY);
        // (Test46d) Turning is the game's own stick turn again, exactly as in Test45 - its body turns with the
        // view and the arms stay right ([DIRECTTURN] wrote Miles's rotation directly: "the right hand is twisted
        // and wrong looking"). Turn speed scales the stick up to the game's own fastest (~258 deg/s).
        const float f = fminf(1.0f, (float)g_turnDegPerSec / kGameTurnDegPerSec) * g_turnSpeed;
        if (g_turnMode) SnapTurn(lookX, now);
        else if (f < 0.999f && fabsf(*lookX) > 0.12f) {
            // Scale only the travel past UE3's right-stick dead zone (~0.27). Scaling the raw
            // stick instead parked every low setting inside the dead zone: no turning at all.
            const float dz = 0.27f, a = fabsf(*lookX);
            const float t = (a - 0.12f) / 0.88f;   // 0 at the inject gate .. 1 at full stick
            *lookX = (*lookX > 0.0f ? 1.0f : -1.0f) * (dz + (1.0f - dz) * t * f);
        }
        if (!g_stickPitch && !camcorderUp) *lookY = LevelPitch(now);
    } else {
        SnapReset();
        OLUE3_TurnRate(0.0f);
    }
    return PhysicalCrouch(play, now);
}
