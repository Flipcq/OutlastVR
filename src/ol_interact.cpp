// ol_interact.cpp - [INTERACT] (Test37/38) native-VR interactions
// =============================================================================
// DOORS     Squeeze grip on a door knob and the door swings with your hand; let go to leave it.
//           (Test39) Or just push: an open hand against the door panel moves it as far as your hand
//           goes, slowly or all at once, Boneworks-style. A hand on a handle closes into a grip.
//           OLDoor's own tick follows TargetOpenRatio (proven live: writing it opened a door
//           smoothly, no Use press, no animation), so the hand only ever sets that. Locked doors
//           buzz and their marker flashes red. Lockers and scripted doors are left to the game.
// HOLSTER   (Test38, hip since Test39) The camcorder lives on your right hip. Reach there with the right hand and squeeze:
//           it glides into your hand (the game's own handycam model, placed by ol_ue3 [HOLSTER]).
//           Bring it to your eye and you look through it; take it away and it is back in your
//           hand. Open your hand anywhere and it glides back to the hip.
//           Without the holster ([Motion] CamcorderHolster=0) Test37's raise-to-eye gesture and
//           the right-grip toggle are used instead.
// RELOAD    (Test38, holster) With the camcorder in your right hand: the left hand pulls the old
//           battery out of the back of it and drops it, takes a new one from the left hip and
//           slides it in - the game's own reload runs (Y). Y still reloads at any time.
//           Without the holster, Test37's hip-to-hand reload is used.
// HOW-TO    (Test38) Labels and glows in the headset show what to do next. Each label stops after
//           you have done that action 3 times ([Motion] Learned*); the glows stay.
// MARKERS   Developer rings for every zone ([Motion] DevMarkers).
// (Test40) DOORS are heavy hinged bodies ([DOORPHYS] below); CLIMBING with your hands ([CLIMB]):
//           hold the edge, pull; hang, pull up or let go; ladders rung by rung. The hands that hold
//           an edge, a handle or the panel they push stay there ([ANCHOR], OLInteract_HandAnchorWorld).
// (Test43) [PHYSCLIMB] climbing is yours: your hands carry Miles's body 1:1 (ClimbMode=2; 1 = the
//           Test41 guided climb, ClimbWithHands=0 = the A button). [DOOREVENT] a door the hands open
//           fires the game's own door events (its scares). [BREATH] cover your mouth to hold his breath.
//
// Coordinates. The tracked hands and head arrive in the recenter frame (metres, +x right, +y up,
// +z back). The camera hook records, per frame, the head-centre WORLD position and the body yaw
// the view was built from; the same mapping the arm IK uses then carries a hand into the world
// at 100 uu per metre (measured, see kMetersToUU in ol_ue3.cpp) and a world point back out.
// Everything here runs on the game thread: the tick at the end of the frame's CPU split, the pad
// merge from the XInput poll (also the game thread), so no state below needs a lock.
// =============================================================================
#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include "ol_xr.h"
#include "ol_interact.h"
#include "ol_menu.h"

extern "C" void  OLProxyLog(const char* line);
extern "C" int   OLUE3_FindProp(const char* cls, const char* prop, unsigned* off, unsigned* mask);
extern "C" void* OLUE3_Hero();
extern "C" void* OLUE3_PlayerController();
extern "C" int   OLUE3_ObjectsOfClass(const char* cls, void** out, int max);
extern "C" int   OLUE3_FrameView(float head[3], float* baseYaw);
extern "C" int   OLUE3_DoorKnobWorld(void* door, float out[3]);
extern "C" int   OLUE3_DoorHinge(void* door, float out[3]);
extern "C" int   OLUE3_GameplayActive();
extern "C" int   OLUE3_InGameMenu();
extern "C" int   OLUE3_HolsterVisual(float out[3]);   // where the holstered model is drawn
extern "C" int   OLUE3_HeroMoveState(int* lm, int* sm);                 // (Test40) LocomotionMode, SpecialMove
extern "C" int   OLUE3_DoorSetRatioAndTarget(void* door, float r);      // (Test40) 0xCC46A0
extern "C" int   OLUE3_CameraBoneWorld(float out[3]);                   // (Test40) for the anchor check
extern "C" void  OLUE3_SetHeroAnimRate(float r);                        // (Test41) [CLIMBPACE]
extern "C" int   OLUE3_HeroMoveReady();                                 // (Test43) [PHYSCLIMB]
extern "C" int   OLUE3_HeroMove(const float d[3], float out[3]);
extern "C" int   OLUE3_HeroSetPhysics(int phys);
extern "C" int   OLUE3_DoorEventHook(void* door);                        // (Test43) [DOOREVENT]
extern "C" int   OLUE3_DoorTriggerEvent(void* door, int type);
extern "C" void  OLUE3_DevCamHold(int mode);                             // (Test43) [CAMHOLD]
extern "C" void  OLUE3_SetFlipScreenOn(int on);                          // (Test43) [FLIPSCREEN]
extern "C" int   OLSound_Ready();                                        // (Test43) [BREATH] ol_sound.cpp
extern "C" int   OLSound_BreathHeavy();
extern "C" void  OLSound_Hush(int on);
extern "C" void  OLSound_Gasp();
extern "C" void  OLSound_Report();
extern "C" void  OLSound_PostByName(const char* name, uint64_t obj);  // [DOOR] post a Wwise door sound by name
extern "C" void  OLSound_StopByName(const char* name, uint64_t obj);  // [DOOR] stop it early (the closing latch)
extern "C" void  OLSound_SetRTPCByName(const char* name, float v);     // [DOOR] choose the slow/calm door sound
extern "C" void  OLProxy_SetNvRequested(int on);                        // (Test43) [HANDNV] d3d9_proxy's night-vision state
extern "C" int   OLUE3_SceneScripted();                                  // (Test44) [ROOMWALK]
extern "C" void  OLUE3_ObjScanStep();                                    // (Test45) [OBJSCAN]
extern "C" void  OLUE3_SetCamAdjust(const float rot[3], const float off[3]);   // (Test45) [CAMADJUST]
extern "C" void  OLUE3_SetLiveScreen(int on);                                       // (Test46) [LIVESCREEN]
extern "C" int   OLUE3_GetHeadPos();

static void ILog(const char* fmt, ...) {
    char b[400]; va_list a; va_start(a, fmt); vsprintf_s(b, fmt, a); va_end(a); OLProxyLog(b);
}

// ---- settings ([Motion] in outlastvr.ini) ---------------------------------------------------------
enum { ON_DOORS, ON_RAISE, ON_BATTERY, ON_VIEWFINDER, ON_MARKERS, ON_PROMPTS, ON_HOLSTER, ON_HOWTO, ON_CLIMB,
       ON_SCREEN, ON_BREATH, ON_ROOMWALK, ON_VFFRAME, ON_AUTOLEDGE, ON_COUNT };   // (Beta 1.1) ON_AUTOLEDGE   // (Test46) the framed zoom viewfinder   // (Test43) the camcorder's live screen, cover your mouth; (Test44) walk around for real
// Beta defaults (Test39): reload is the Y button (the hands-on reload stays as an option), the
// developer rings are off.
// (Test40) climbing with your hands is on by default ([Motion] ClimbWithHands=0: A climbs, as before).
// (Test45) the live screen (a compositor overlay, not the model's own screen) and cover-your-mouth are off.
// (Test46) the live screen is ON again: it shows the camcorder lens's own picture now ([LIVESCREEN] ol_ue3 +
// [LENSCAM] ol_xr), a second camera that runs only while the camcorder is in your hand facing you.
// (Test46c) the user: "remove the live screen for now" - off by default and out of the menus again.
static const int kOnDefault[ON_COUNT] = { 1, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0 };
static volatile long g_on[ON_COUNT] = { 1, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0 };
static const wchar_t* kKeys[ON_COUNT] = { L"DoorGrab", L"CamcorderRaiseGesture", L"BatteryGesture",
                                          L"CamcorderViewfinder", L"DevMarkers", L"QuestPrompts",
                                          L"CamcorderHolster", L"HowToHints", L"ClimbWithHands",
                                          L"CamcorderLiveScreen", L"CoverMouth", L"RoomscaleWalk", L"ViewfinderFrame",
                                          L"AutoLedgeGrab" };
// (Test43) [Motion] ClimbMode: 2 = physical (your hands carry Miles, default), 1 = guided (Test41: the
// game's climb, paced by your pull). ClimbWithHands=0 still means the A button. [Motion] CamcorderHold:
// 4 = free hold (default), 3 = the game's raise pose (Test42).
static volatile long g_climbMode = 2, g_camHold = 4;   // (Beta 1.1) back to 4: the real-camcorder hold (5) was broken
static volatile long g_camHud = 1;        // [CAMHUD] [Render] CamcorderHud
// (Test48) [RUNGRIP] the left grip is also sprint. Pushing the left stick, with the left hand down at the hip or
// waist (0.45 m or more below the head), a fresh squeeze is a sprint, not a hold on the table, counter or railing
// edge beside you - a hold stops sprint and hands Miles to your hands ("most of the time running ... does not
// work"). A raised hand still takes an edge, and a ladder rung is always taken. [Motion] RunGripIgnoresLowEdges=0
// = as before.
static volatile long g_runGripLow = 1;
// (Test49) [CAMBUTTON] [Motion] CamcorderControl: 0 = hands-on (default: take it from the right hip, bring it to
// your eye), 1 = GRIP BUTTON - one squeeze of the right grip raises the camcorder, the next lowers it (the game's
// own toggle), and you look through it with your head, so the hand can rest anywhere: for seated players and
// arms that tire of being held up. The hip, the raise-to-eye gesture and right-hand door and edge grabs are off
// then - the right grip is only the camcorder button. The hands-on settings are left as they are, so switching
// back restores them.
static volatile long g_camButton = 0;
extern "C" int  OLInteract_CamButton() { return g_camButton ? 1 : 0; }
extern "C" void OLInteract_SetCamButton(int on) {
    g_camButton = on ? 1 : 0;
    ILog("[OLVR][CAMBUTTON] camcorder controls: %s", g_camButton ? "GRIP BUTTON (squeeze to raise, again to lower)" : "hands-on");
}
// (Test45) [CAMADJUST] the camcorder in your hand: tilt, turn, roll (deg); forward, up, right (cm)
static float g_camAdjRot[3] = { 0.0f, 0.0f, 0.0f }, g_camAdjOff[3] = { 0.0f, 0.0f, 0.0f };
static const wchar_t* kCamAdjKeys[6] = { L"CamcorderTilt", L"CamcorderTurn", L"CamcorderRoll", L"CamcorderForwardCM", L"CamcorderUpCM", L"CamcorderRightCM" };
extern "C" void OLInteract_GetCamAdjust(float rot[3], float off[3]) { for (int i = 0; i < 3; ++i) { rot[i] = g_camAdjRot[i]; off[i] = g_camAdjOff[i]; } }
extern "C" void OLInteract_SetCamAdjust(const float rot[3], const float off[3]) {
    for (int i = 0; i < 3; ++i) {
        g_camAdjRot[i] = rot[i] < -180.0f ? -180.0f : (rot[i] > 180.0f ? 180.0f : rot[i]);
        g_camAdjOff[i] = off[i] < -30.0f ? -30.0f : (off[i] > 30.0f ? 30.0f : off[i]);
    }
    OLUE3_SetCamAdjust(g_camAdjRot, g_camAdjOff);
}
// How close (m) the right controller has to come to the eye zone to raise the camcorder. The grip
// pose sits in the handle, so a controller held against the headset is ~5 cm from the zone and one
// held a hand's width out ~12 cm.
static float g_eyeReach = 0.14f;
// [HOWTO] how often each action has been done; its label stops at kLearnDone
enum { LN_TAKECAM, LN_RAISE, LN_EJECT, LN_INSERT, LN_DOOR, LN_CLIMB, LN_BREATH, LN_RECENTER, LN_COUNT };
static const wchar_t* kLearnKeys[LN_COUNT] = { L"LearnedTakeCamcorder", L"LearnedRaise", L"LearnedEject",
                                               L"LearnedInsert", L"LearnedDoor", L"LearnedClimb", L"LearnedCoverMouth",
                                               L"LearnedRecenter" };
static const int kLearnDone = 3;
static int g_learn[LN_COUNT] = {};
extern "C" int  OLInteract_Get(int w) { return (w >= 0 && w < ON_COUNT && g_on[w]) ? 1 : 0; }
extern "C" void OLInteract_Set(int w, int on) {
    if (w >= 0 && w < ON_COUNT) g_on[w] = on ? 1 : 0;
    if (w == ON_SCREEN) OLUE3_SetFlipScreenOn(on);
}
// (Test43) 0 = the A button, 1 = guided, 2 = physical
extern "C" int OLInteract_ClimbMode() { return g_on[ON_CLIMB] ? (int)g_climbMode : 0; }
extern "C" void OLInteract_SetClimbMode(int m) {
    if (m <= 0) g_on[ON_CLIMB] = 0;
    else { g_on[ON_CLIMB] = 1; g_climbMode = m >= 2 ? 2 : 1; }
}
extern "C" int OLInteract_CamHold() { return (int)g_camHold; }
extern "C" void OLInteract_SetCamHold(int m) { g_camHold = m == 3 ? 3 : 4; OLUE3_DevCamHold((int)g_camHold); }
static bool IniPath(wchar_t* out, size_t n) {
    wchar_t exe[MAX_PATH] = { 0 };
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    *(slash + 1) = 0;
    swprintf_s(out, n, L"%soutlastvr.ini", exe);
    return true;
}
void OLInteract_Init() {
    wchar_t ini[MAX_PATH];
    if (!IniPath(ini, MAX_PATH)) return;
    for (int i = 0; i < ON_COUNT; ++i) g_on[i] = GetPrivateProfileIntW(L"Motion", kKeys[i], kOnDefault[i], ini) ? 1 : 0;
    for (int i = 0; i < LN_COUNT; ++i) g_learn[i] = GetPrivateProfileIntW(L"Motion", kLearnKeys[i], 0, ini);
    wchar_t v[32];
    GetPrivateProfileStringW(L"Motion", L"RaiseToEyeReachM", L"0.14", v, 32, ini);
    g_eyeReach = (float)_wtof(v);
    if (!(g_eyeReach >= 0.05f && g_eyeReach <= 0.25f)) g_eyeReach = 0.14f;
    // ClimbMode, when there, decides (the launcher writes it); an older ini only has ClimbWithHands
    const int cm = GetPrivateProfileIntW(L"Motion", L"ClimbMode", -1, ini);
    if (cm == 0) g_on[ON_CLIMB] = 0;
    else if (cm == 1 || cm == 2) { g_on[ON_CLIMB] = 1; g_climbMode = cm; }
    else g_climbMode = 2;
    g_camHold = GetPrivateProfileIntW(L"Motion", L"CamcorderHold", 4, ini) == 3 ? 3 : 4;   // (Beta 1.1) a saved 5 means 4
    OLInteract_SetCamcorderHud(GetPrivateProfileIntW(L"Render", L"CamcorderHud", 1, ini));
    g_runGripLow = GetPrivateProfileIntW(L"Motion", L"RunGripIgnoresLowEdges", 1, ini) ? 1 : 0;   // (Test48)
    OLInteract_SetCamButton(GetPrivateProfileIntW(L"Motion", L"CamcorderControl", 0, ini));        // (Test49)
    ILog("[OLVR][RUNGRIP] a running squeeze at the hip is sprint, not an edge: %s", g_runGripLow ? "on" : "off");
    OLUE3_DevCamHold((int)g_camHold);
    {   // (Test45) [CAMADJUST]
        float r[3], o[3];
        for (int i = 0; i < 6; ++i) { GetPrivateProfileStringW(L"Motion", kCamAdjKeys[i], L"0", v, 32, ini); (i < 3 ? r[i] : o[i - 3]) = (float)_wtof(v); }
        OLInteract_SetCamAdjust(r, o);
    }
    OLUE3_SetFlipScreenOn(g_on[ON_SCREEN]);
    ILog("[OLVR][INTERACT] doors=%ld holster=%ld raiseToEye=%ld (reach %.2f m) battery=%ld viewfinder=%ld howTo=%ld "
         "devMarkers=%ld questPrompts=%ld climbWithHands=%ld climbMode=%ld camHold=%ld liveScreen=%ld coverMouth=%ld roomscaleWalk=%ld | learned take=%d raise=%d eject=%d insert=%d door=%d climb=%d breath=%d",
         g_on[ON_DOORS], g_on[ON_HOLSTER], g_on[ON_RAISE], g_eyeReach, g_on[ON_BATTERY], g_on[ON_VIEWFINDER],
         g_on[ON_HOWTO], g_on[ON_MARKERS], g_on[ON_PROMPTS], g_on[ON_CLIMB], g_climbMode, g_camHold, g_on[ON_SCREEN], g_on[ON_BREATH], g_on[ON_ROOMWALK],
         g_learn[0], g_learn[1], g_learn[2], g_learn[3], g_learn[4], g_learn[5], g_learn[6]);
}
void OLInteract_Save(const wchar_t* ini) {
    for (int i = 0; i < ON_COUNT; ++i) WritePrivateProfileStringW(L"Motion", kKeys[i], g_on[i] ? L"1" : L"0", ini);
    wchar_t b[32]; swprintf_s(b, L"%.3f", g_eyeReach); WritePrivateProfileStringW(L"Motion", L"RaiseToEyeReachM", b, ini);
    swprintf_s(b, L"%ld", g_on[ON_CLIMB] ? g_climbMode : 0L); WritePrivateProfileStringW(L"Motion", L"ClimbMode", b, ini);
    swprintf_s(b, L"%ld", g_camHold); WritePrivateProfileStringW(L"Motion", L"CamcorderHold", b, ini);
    WritePrivateProfileStringW(L"Render", L"CamcorderHud", g_camHud ? L"1" : L"0", ini);   // (Beta 1.1)
    WritePrivateProfileStringW(L"Motion", L"CamcorderControl", g_camButton ? L"1" : L"0", ini);   // (Test49)
    for (int i = 0; i < 6; ++i) { swprintf_s(b, L"%.0f", i < 3 ? g_camAdjRot[i] : g_camAdjOff[i - 3]); WritePrivateProfileStringW(L"Motion", kCamAdjKeys[i], b, ini); }
    for (int i = 0; i < LN_COUNT; ++i) { swprintf_s(b, L"%d", g_learn[i]); WritePrivateProfileStringW(L"Motion", kLearnKeys[i], b, ini); }
}
static void Learn(int what) {
    if (what < 0 || what >= LN_COUNT || g_learn[what] >= kLearnDone) return;
    ++g_learn[what];
    wchar_t ini[MAX_PATH], b[16];
    if (IniPath(ini, MAX_PATH)) { swprintf_s(b, L"%d", g_learn[what]); WritePrivateProfileStringW(L"Motion", kLearnKeys[what], b, ini); }
    if (g_learn[what] == kLearnDone) ILog("[OLVR][HOWTO] '%ls' learned - its label is hidden from now on", kLearnKeys[what]);
}
static bool Teach(int what) { return g_on[ON_HOWTO] && g_learn[what] < kLearnDone; }
extern "C" void OLInteract_ResetHints() {
    for (int i = 0; i < LN_COUNT; ++i) g_learn[i] = 0;
    wchar_t ini[MAX_PATH];
    if (IniPath(ini, MAX_PATH)) for (int i = 0; i < LN_COUNT; ++i) WritePrivateProfileStringW(L"Motion", kLearnKeys[i], L"0", ini);
    OLProxyLog("[OLVR][HOWTO] how-to labels reset - they show again until each action is done 3 times");
}

// ---- small maths ---------------------------------------------------------------------------------
struct V3 { float x, y, z; };
static V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 Mul(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static float Len(V3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static V3 QRot(const OLXrPose& q, V3 v) {
    const V3 u = { q.qx, q.qy, q.qz };
    const V3 c1 = { u.y * v.z - u.z * v.y + q.qw * v.x, u.z * v.x - u.x * v.z + q.qw * v.y, u.x * v.y - u.y * v.x + q.qw * v.z };
    const V3 c2 = { u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x };
    return { v.x + 2.0f * c2.x, v.y + 2.0f * c2.y, v.z + 2.0f * c2.z };
}
static float Wrap180(float d) { while (d > 180.0f) d -= 360.0f; while (d < -180.0f) d += 360.0f; return d; }
static V3 P3(const OLXrPose& p) { return { p.px, p.py, p.pz }; }

// The frame's mapping between the recenter frame and the world.
static V3 s_headT, s_headW;       // tracked head (m) and its world position (uu)
static float s_cy = 1.0f, s_sy = 0.0f;
static V3 TrackToWorld(V3 t) {
    const V3 d = Sub(t, s_headT);
    const float f = -d.z * 100.0f, r = d.x * 100.0f, u = d.y * 100.0f;
    return { s_headW.x + s_cy * f - s_sy * r, s_headW.y + s_sy * f + s_cy * r, s_headW.z + u };
}
static V3 WorldToTrack(V3 w) {
    const V3 d = Mul(Sub(w, s_headW), 0.01f);
    const float f = d.x * s_cy + d.y * s_sy, r = -d.x * s_sy + d.y * s_cy;
    return { s_headT.x + r, s_headT.y + d.z, s_headT.z - f };
}

// ---- engine fields, by reflection -----------------------------------------------------------------
static struct {
    bool ok; ULONGLONG nextTry; int tries;
    unsigned camState, zoom, energy, numBat, loc;
    unsigned openR, tgtR, maxAng, dtype, bits, mLocked, mFake, mRev, mCant;
    // (Test40) optional: the door's own state, and the ledge markers
    unsigned dstate, dbreak, duser, mBusyBits;
    unsigned lmNext, lmBits, mClimbUp, mHang, gmBits, mEnabled;
    unsigned ladOther, rot;                                  // (Test41) ladders
    unsigned phys, vel, cyl, cylH, cylR;                     // (Test43) [PHYSCLIMB] Physics, Velocity, the collision cylinder
    unsigned camMode;                                        // (Test43) [HANDNV] OLHero.CamcorderMode (0 default, 1/2 night vision)
} F = {};
static bool Resolve() {
    if (F.ok) return true;
    const ULONGLONG now = GetTickCount64();
    if (now < F.nextTry || F.tries >= 20) return false;
    F.nextTry = now + 3000; ++F.tries;
    bool ok = OLUE3_FindProp("OLHero", "CamcorderState", &F.camState, nullptr) &&
              OLUE3_FindProp("OLHero", "CurrentCamcorderZoomFactor", &F.zoom, nullptr) &&
              OLUE3_FindProp("OLHero", "CurrentBatterySetEnergy", &F.energy, nullptr) &&
              OLUE3_FindProp("OLPlayerController", "NumBatteries", &F.numBat, nullptr) &&
              OLUE3_FindProp("Actor", "Location", &F.loc, nullptr) &&
              OLUE3_FindProp("OLDoor", "OpenRatio", &F.openR, nullptr) &&
              OLUE3_FindProp("OLDoor", "TargetOpenRatio", &F.tgtR, nullptr) &&
              OLUE3_FindProp("OLDoor", "MaxOpenAngle", &F.maxAng, nullptr) &&
              OLUE3_FindProp("OLDoor", "DoorType", &F.dtype, nullptr) &&
              OLUE3_FindProp("OLDoor", "bLocked", &F.bits, &F.mLocked);
    unsigned b2 = 0;
    if (ok) {
        OLUE3_FindProp("OLDoor", "bFakeUnlocked", &b2, &F.mFake);
        OLUE3_FindProp("OLDoor", "bReverseDirection", &b2, &F.mRev);
        OLUE3_FindProp("OLDoor", "bCantClose", &b2, &F.mCant);
        // (Test40) [DOORPHYS] when the game is doing something with a door itself
        OLUE3_FindProp("OLDoor", "DoorState", &F.dstate, nullptr);
        OLUE3_FindProp("OLDoor", "DoorBreakState", &F.dbreak, nullptr);
        OLUE3_FindProp("OLDoor", "DoorUser", &F.duser, nullptr);
        const char* busy[] = { "bPlayingLockedAnim", "bPlayingBlockedAnim", "bPlayingAutoCloseAnim", "bAITraversing" };
        for (const char* nm : busy) { unsigned m = 0; if (OLUE3_FindProp("OLDoor", nm, &b2, &m)) F.mBusyBits |= m; }
        // (Test40) [CLIMB] the edges you can climb
        if (OLUE3_FindProp("OLLedgeMarker", "Next", &F.lmNext, nullptr)) {
            OLUE3_FindProp("OLLedgeMarker", "bCanClimbUp", &F.lmBits, &F.mClimbUp);
            OLUE3_FindProp("OLLedgeMarker", "bCanLedgeHang", &b2, &F.mHang);
            OLUE3_FindProp("OLGameplayMarker", "bEnabled", &F.gmBits, &F.mEnabled);
        }
        // (Test41) [LADDERGRAB] ladders: a bottom and a top OLLadderMarker pointing at each other
        OLUE3_FindProp("OLLadderMarker", "OtherMarker", &F.ladOther, nullptr);
        OLUE3_FindProp("Actor", "Rotation", &F.rot, nullptr);
        // (Test43) [PHYSCLIMB] Miles's physics mode and velocity, and his collision cylinder's size
        OLUE3_FindProp("Actor", "Physics", &F.phys, nullptr);
        OLUE3_FindProp("Actor", "Velocity", &F.vel, nullptr);
        OLUE3_FindProp("Pawn", "CylinderComponent", &F.cyl, nullptr);
        OLUE3_FindProp("CylinderComponent", "CollisionHeight", &F.cylH, nullptr);
        OLUE3_FindProp("CylinderComponent", "CollisionRadius", &F.cylR, nullptr);
        OLUE3_FindProp("OLHero", "CamcorderMode", &F.camMode, nullptr);
        ILog("[OLVR][PHYSCLIMB] Physics+0x%X Velocity+0x%X CylinderComponent+0x%X (height+0x%X radius+0x%X)", F.phys, F.vel, F.cyl, F.cylH, F.cylR);
        ILog("[OLVR][INTERACT] (Test40) door state+0x%X break+0x%X user+0x%X busyBits=0x%X | ledge next+0x%X bits+0x%X climbUp=0x%X hang=0x%X enabled+0x%X/0x%X",
             F.dstate, F.dbreak, F.duser, F.mBusyBits, F.lmNext, F.lmBits, F.mClimbUp, F.mHang, F.gmBits, F.mEnabled);
    }
    F.ok = ok;
    ILog("[OLVR][INTERACT] fields %s (try %d): camState+0x%X zoom+0x%X energy+0x%X numBat+0x%X | door open+0x%X target+0x%X "
         "max+0x%X type+0x%X bits+0x%X locked=0x%X fake=0x%X reverse=0x%X cantClose=0x%X",
         ok ? "resolved" : "MISSING", F.tries, F.camState, F.zoom, F.energy, F.numBat, F.openR, F.tgtR,
         F.maxAng, F.dtype, F.bits, F.mLocked, F.mFake, F.mRev, F.mCant);
    return ok;
}
template <class T> static bool Rd(const void* base, unsigned off, T* out) {
    if (!base || !off) return false;
    __try { *out = *(const T*)((const char*)base + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool WrF(void* base, unsigned off, float v) {
    if (!base || !off) return false;
    __try { *(volatile float*)((char*)base + off) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- shared per-frame state ------------------------------------------------------------------------
static ULONGLONG s_tickMs = 0;              // last tick; the pad merge stops acting when this goes stale
static int   s_camUp = -1;                  // the game's CamcorderState (1 = raised)
static bool  s_camStarted = false;          // the first gameplay tick forced the camcorder down
static float s_zoom = 0.0f, s_energy = 1.0f;
static volatile float s_vfZoom = 0.0f;
static volatile ULONGLONG s_vfMs = 0;
static int   s_numBat = 0, s_prevNumBat = -1;
// [CAMHUD] XR and gameplay may run on different threads. Publish the whole snapshot together.
static SRWLOCK s_hudLock = SRWLOCK_INIT;
static OLCamcorderHudState s_hudState;
static ULONGLONG s_hudPublished = 0;
static volatile LONG64 s_hudSubmitted = 0;
extern "C" int OLUE3_CamcorderHudRead(float* seconds, int* capacity, int hide);
// (Beta 1.1.3) night vision is the proxy's own camcorder state (raised + night vision requested), which
// [HANDNV] keeps in step with the game's CamcorderMode. The HUD's eye and speaker tell-tales follow it.
extern "C" int OLProxy_NightVisionActive();
extern "C" int OLInteract_CamcorderHud(OLCamcorderHudState* state) {
    AcquireSRWLockShared(&s_hudLock);
    *state = s_hudState;
    const bool fresh = s_hudState.visible && GetTickCount64() - s_hudPublished < 1000;
    ReleaseSRWLockShared(&s_hudLock);
    return g_camHud && fresh;
}
extern "C" int OLInteract_GetCamcorderHud() { return g_camHud ? 1 : 0; }
extern "C" void OLInteract_SetCamcorderHud(int on) {
    g_camHud = on ? 1 : 0;
    OLProxyLog(g_camHud ? "[OLVR][CAMHUD] custom HUD enabled" : "[OLVR][CAMHUD] custom HUD disabled");
}
extern "C" void OLInteract_CamcorderHudSubmitted(int shown) {
    InterlockedExchange64(&s_hudSubmitted, shown ? (LONG64)GetTickCount64() : 0);
}
static void CamcorderHudTick(bool active) {
    OLCamcorderHudState state;
    const ULONGLONG submitted = (ULONGLONG)InterlockedCompareExchange64(&s_hudSubmitted, 0, 0);
    const bool wanted = g_camHud && active && s_camUp == 1 && !OLUE3_InGameMenu() && !OLMenu_Visible();
    const bool ready = OLUE3_CamcorderHudRead(&state.recordingSeconds, &state.capacity,
        wanted && submitted && GetTickCount64() - submitted < 500);
    state.energy = s_energy; state.zoom = s_zoom; state.spares = s_numBat;
    state.nightVision = OLProxy_NightVisionActive() != 0;
    state.visible = wanted && ready;
    const int status = !g_camHud ? 0 : !active ? 1 : s_camUp != 1 ? 2 : !wanted ? 3 : !ready ? 4 : 5;
    static int lastStatus = -1;
    if (status != lastStatus) {
        static const char* messages[] = {
            "[OLVR][CAMHUD] inactive: setting off", "[OLVR][CAMHUD] waiting for gameplay/data",
            "[OLVR][CAMHUD] hidden: camcorder lowered", "[OLVR][CAMHUD] hidden: menu open",
            "[OLVR][CAMHUD] waiting for camcorder movie/clock", "[OLVR][CAMHUD] publishing camcorder HUD"
        };
        OLProxyLog(messages[status]); lastStatus = status;
    }
    AcquireSRWLockExclusive(&s_hudLock);
    s_hudState = state; s_hudPublished = GetTickCount64();
    ReleaseSRWLockExclusive(&s_hudLock);
}
static bool  s_gripDown[2] = { false, false };
static bool  s_gripEdge[2] = { false, false };
// requests to the pad merge
static ULONGLONG s_camPulseUntil = 0, s_camCooldown = 0, s_reloadUntil = 0;
static bool  s_blockGrip[2] = { false, false };
static bool  s_nearKnob[2] = { false, false };
static bool  s_nearDoorWide[2] = { false, false };   // near a knob: the right grip is the door's, not the camcorder's
static bool  s_inPouch = false, s_moving = false;
static V3    s_anchorW[2] = {};                // [ANCHOR] (Test40) where each hand holds on (world)
static float s_anchorWt[2] = { 0.0f, 0.0f };    // eased weight

// ---- the body's facing (Test38) ------------------------------------------------------------------
// Nothing tracks your torso, so it is estimated from the head: it stays put while you look around
// within 35 degrees either side and is dragged along beyond that. The chest holster and the hip
// pouch hang off it, so glancing sideways no longer swings them round with your head.
static V3 s_torsoF = { 0.0f, 0.0f, -1.0f };
static bool s_torsoValid = false;
static void TorsoUpdate(const OLXrPose& head) {
    V3 f = QRot(head, { 0.0f, 0.0f, -1.0f }); f.y = 0.0f;
    const float fl = sqrtf(f.x * f.x + f.z * f.z);
    if (fl < 0.2f) return;                         // looking straight up or down: keep the last facing
    f = Mul(f, 1.0f / fl);
    if (!s_torsoValid) { s_torsoF = f; s_torsoValid = true; return; }
    const float aH = atan2f(-f.x, -f.z), aT = atan2f(-s_torsoF.x, -s_torsoF.z);
    float d = aH - aT;
    while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f;
    const float lim = 35.0f * 3.14159265f / 180.0f;
    if (d > lim) d -= lim; else if (d < -lim) d += lim; else return;
    const float a = aT + d;
    s_torsoF = { -sinf(a), 0.0f, -cosf(a) };
}
static V3 TorsoRight() { return { -s_torsoF.z, 0.0f, s_torsoF.x }; }
static V3 OnBody(float right, float up, float fwd) {
    return Add(s_headT, Add(Mul(TorsoRight(), right), Add(V3{ 0.0f, up, 0.0f }, Mul(s_torsoF, fwd))));
}

// ---- camcorder: holster, raise to your eye ---------------------------------------------------------
static bool s_camByGesture = false;         // the mod raised it (and lowers it again)
static ULONGLONG s_inEyeSince = 0;
// One raise attempt per time the hand comes to the eye: a reload (or a cutscene) lowers the camcorder
// while the hand is still there, and retrying every cooldown fought the game's own animation.
static bool s_eyeArmed = true;
static ULONGLONG s_gesturePauseUntil = 0;   // after a reload, while the game swaps the battery
static bool s_rearmAfterPause = false;       // it was up when you reloaded: bring it back up once
static V3 s_eyeZone;
static float s_dEye = 9.0f;
// (Beta 1.1.4) [FACECAM] Hold the right stick click and the camcorder is raised by the game and
// stays at Miles's face with the hand resting; the game's own arm plays its raise pose. Another
// held click, or a squeeze at the face, runs the game's lower animation. A held click while it is
// already up only pins it (no animation) and buzzes the right controller.
static bool s_faceLock = false;              // the game's camcorder is up and pinned at his face
static bool s_facePending = false;           // a held click asked for the raise; waiting for the game
static bool s_faceLowering = false;          // a held right stick click asked for the lower
static ULONGLONG s_facePendingAt = 0, s_faceLowerAt = 0;
// enter at the reach, lower 8 cm further out, re-arm 7 cm out
static float EyeEnter() { return g_eyeReach; }
static float EyeLeave() { return g_eyeReach + 0.08f; }
static void RequestCamToggle(ULONGLONG now, ULONGLONG pulseMs) { s_camPulseUntil = now + pulseMs; s_camCooldown = now + pulseMs + 500; }

// [HOLSTER] (Test39) Right hip: 20 cm right of the head's centre line, 60 cm below the eyes.
static const V3 kHolsterOff = { 0.20f, -0.60f, 0.0f };    // right, up, forward (torso frame)
// (Beta 1.1.4) [WAIST] The grab area is a waist-shaped box, not a 16 cm ball: a hand coming down
// or across to the hip, and a hand resting in the lap when you play seated, both count. Measured
// in the torso frame around the holster (and around the model the game draws, below).
static const float kWaistSideM = 0.26f, kWaistUpM = 0.20f, kWaistDownM = 0.32f,
                   kWaistFwdM = 0.34f, kWaistBackM = 0.20f;
static bool InWaistZone(V3 p, V3 center) {
    const V3 d = Sub(p, center);
    const V3 r = TorsoRight();
    const float side = d.x * r.x + d.z * r.z;
    const float fwd = d.x * s_torsoF.x + d.z * s_torsoF.z;
    const float ns = side / kWaistSideM;
    const float nu = d.y / (d.y >= 0.0f ? kWaistUpM : kWaistDownM);
    const float nf = fwd / (fwd >= 0.0f ? kWaistFwdM : kWaistBackM);
    return ns * ns + nu * nu + nf * nf <= 1.0f;
}
enum { H_HOLSTERED = 0, H_IN_HAND };
static int s_hState = H_HOLSTERED;
static float s_camBlend = 0.0f;              // 0 on the chest .. 1 in the hand (animated)
static float s_blendSpeed = 5.5f;            // per second
static V3 s_holster, s_holsterVis;          // where you reach (your chest) / where the model is drawn (his)
static bool s_inHolster = false;
static float s_dHolster = 9.0f;
static ULONGLONG s_lastBlendMs = 0;
static bool s_returnClick = false;
static bool HolsterOn() { return g_on[ON_HOLSTER] != 0 && !g_camButton; }   // (Test49) [CAMBUTTON]: no hip

static void CamRaise(ULONGLONG now, const OLXrPose& head, const OLXrFrameHands& fh) {
    s_eyeZone = Add(s_headT, QRot(head, { 0.035f, -0.035f, -0.11f }));   // just in front of the right eye
    const bool rOk = (fh.valid & 2u) != 0;
    s_dEye = rOk ? Len(Sub(P3(fh.hand[1]), s_eyeZone)) : 9.0f;
    if (s_dEye > EyeEnter() + 0.07f) s_eyeArmed = true;                       // the hand left the eye
    // (Beta 1.1.4) [FACECAM] Pinned to the face by a held right stick click: the hand can rest, so
    // the distance to the eye must not lower it.
    // (V1) grabbing the pinned camcorder at the face with the right hand holds it physically - it is
    // not removed. It only comes off his face with a held right stick click (the game's own lower
    // animation) or when the game itself lowers it.
    if (s_faceLock) {
        // (V1) grabbing the pinned camcorder at the face with the right hand takes it off his face
        // and into your hand (physical hold). It becomes visible in the hand instead of disappearing.
        if (!s_faceLowering && s_gripEdge[1] && s_dEye < EyeLeave() && now > s_camCooldown) {
            s_faceLock = false;
            s_faceLowering = false;
            s_eyeArmed = false; s_inEyeSince = 0;
            // Put the camera visibly in the hand when physically grabbed from the face.
            s_hState = H_IN_HAND; s_camBlend = 1.0f; s_blendSpeed = 1.0f / 0.18f;
            OLXR_HapticPulse(1, 0.55f, 40);
            ILog("[OLVR][FACECAM] camcorder taken off his face into your hand (%.2f m)", s_dEye);
        }
        s_inEyeSince = 0;
        return;
    }
    if (s_facePending) { s_inEyeSince = 0; return; }   // the game's raise is playing
    const bool holster = HolsterOn();
    const bool allowed = holster ? (s_hState == H_IN_HAND) : (g_on[ON_RAISE] != 0 && !g_camButton);   // (Test49) no gesture
    if (!allowed || s_camUp < 0) { s_inEyeSince = 0; return; }
    if (now < s_gesturePauseUntil) { s_inEyeSince = 0; return; }
    if (s_rearmAfterPause) { s_rearmAfterPause = false; s_eyeArmed = true; }
    if (s_camUp == 0) {
        if (s_camByGesture && now > s_camCooldown) s_camByGesture = false;   // lowered some other way
        if (s_dEye < EyeEnter() && s_eyeArmed) {
            if (!s_inEyeSince) s_inEyeSince = now;
            else if (now - s_inEyeSince > 140 && now > s_camCooldown) {
                RequestCamToggle(now, 120); s_camByGesture = true; s_inEyeSince = 0; s_eyeArmed = false;
                OLXR_HapticPulse(1, 0.55f, 40);
                Learn(LN_RAISE);
                ILog("[OLVR][INTERACT] camcorder RAISED by lifting the right hand to the eye (%.2f m)", s_dEye);
            }
        } else s_inEyeSince = 0;
    } else if ((holster || s_camByGesture) && s_dEye > EyeLeave() && now > s_camCooldown) {
        RequestCamToggle(now, 120); s_camByGesture = false;
        OLXR_HapticPulse(1, 0.35f, 30);
        ILog("[OLVR][INTERACT] camcorder LOWERED (right hand left the eye, %.2f m)", s_dEye);
    }
}

// (Beta 1.1.4) [FACECAM] Hold the right stick click for OLVR_CAM_HOLD_MS. Handled on the game
// thread with the frame's pad, but the proxy withholds the game's night-vision press until release
// and only sends it for a short click, so a held click never flips night vision as a side effect.
static bool s_rsDown = false, s_rsLong = false, s_rsShort = false;
static ULONGLONG s_rsDownAt = 0;
static void CamHoldGesture(ULONGLONG now, const OLXrPad& p) {
    const bool rs = (p.buttons & OLXR_BTN_RSTICK) != 0;
    if (rs && !s_rsDown) { s_rsDown = true; s_rsDownAt = now; s_rsLong = false; return; }
    if (rs && s_rsDown) {
        if (s_rsLong || now - s_rsDownAt < OLVR_CAM_HOLD_MS) return;
        s_rsLong = true;
        if (s_camUp < 0) { ILog("[OLVR][FACECAM] right stick held, but the game's camcorder state is not known yet"); return; }
        if (s_camUp == 1) {
            if (s_faceLock) {
                if (!s_faceLowering) {
                    s_faceLowering = true;
                    s_faceLowerAt = now;
                    s_eyeArmed = false; s_inEyeSince = 0;
                    RequestCamToggle(now, 300);   // longer pulse so the game's lower animation has time to play
                    OLXR_HapticPulse(1, 0.65f, 45);
                    ILog("[OLVR][FACECAM] right stick held: the camcorder comes off his face (the game's lower animation)");
                }
            } else {
                s_faceLock = true; s_facePending = false; s_faceLowering = false;
                s_camByGesture = false;
                OLXR_HapticPulse(1, 0.65f, 45);
                ILog("[OLVR][FACECAM] right stick held: the camcorder is pinned at his face (it was already up, no animation)");
            }
        } else {
            s_facePending = true; s_facePendingAt = now; s_faceLowering = false;
            s_camByGesture = false;
            s_eyeArmed = false; s_inEyeSince = 0;
            RequestCamToggle(now, 300);   // longer pulse so the game's raise animation has time to play
            ILog("[OLVR][FACECAM] right stick held: Miles raises the camcorder - it will stay at his face");
        }
        return;
    }
    if (!rs && s_rsDown) {
        s_rsDown = false;
        if (!s_rsLong) s_rsShort = true;   // a short click: still the night-vision switch
        s_rsLong = false;
    }
}

static void Holster(ULONGLONG now, const OLXrFrameHands& fh) {
    s_holster = OnBody(kHolsterOff.x, kHolsterOff.y, kHolsterOff.z);
    float hv[3];
    s_holsterVis = OLUE3_HolsterVisual(hv) ? V3{ hv[0], hv[1], hv[2] } : s_holster;
    const bool rOk = (fh.valid & 2u) != 0;
    // a grab counts at your own chest or at the model you see on Miles's chest
    const float dReal = rOk ? Len(Sub(P3(fh.hand[1]), s_holster)) : 9.0f;
    const float dVis = rOk ? Len(Sub(P3(fh.hand[1]), s_holsterVis)) : 9.0f;
    s_dHolster = dReal < dVis ? dReal : dVis;
    s_inHolster = rOk && (InWaistZone(P3(fh.hand[1]), s_holster) || InWaistZone(P3(fh.hand[1]), s_holsterVis));
    const float dt = s_lastBlendMs ? (float)(now - s_lastBlendMs) * 0.001f : 0.0f;
    s_lastBlendMs = now;
    if (!HolsterOn() || s_camUp < 0) { s_hState = H_HOLSTERED; s_camBlend = 0.0f; return; }
    if (s_hState == H_HOLSTERED) {
        if (s_gripEdge[1] && s_inHolster && !s_nearDoorWide[1]) {
            s_hState = H_IN_HAND; s_blendSpeed = 1.0f / 0.18f; s_returnClick = false;
            s_gripEdge[1] = false;                                   // the squeeze is used: no door grab
            OLXR_HapticPulse(1, 0.5f, 35);
            Learn(LN_TAKECAM);
            ILog("[OLVR][HOLSTER] camcorder taken from the hip holster (%.2f m)", s_dHolster);
        }
        // It went back to the holster while up at your eye: lower it once the last toggle settled.
        if (s_camUp == 1 && s_camByGesture && now > s_camCooldown) {
            RequestCamToggle(now, 120); s_camByGesture = false;
            ILog("[OLVR][HOLSTER] camcorder lowered - it is back on your hip");
        }
    } else if (!s_gripDown[1] || !rOk) {
        s_hState = H_HOLSTERED; s_blendSpeed = 1.0f / 0.28f; s_returnClick = true;
        if (s_camUp == 1 && s_camByGesture && now > s_camCooldown) { RequestCamToggle(now, 120); s_camByGesture = false; }
        OLXR_HapticPulse(1, 0.25f, 20);
        ILog("[OLVR][HOLSTER] camcorder let go - gliding back to the hip holster");
    }
    const float target = s_hState == H_IN_HAND ? 1.0f : 0.0f;
    if (s_camBlend < target) { s_camBlend += dt * s_blendSpeed; if (s_camBlend > target) s_camBlend = target; }
    else if (s_camBlend > target) {
        s_camBlend -= dt * s_blendSpeed;
        if (s_camBlend <= target) {
            s_camBlend = target;
            if (s_returnClick) { s_returnClick = false; OLXR_HapticPulse(1, 0.45f, 18); }   // it clicks into place
        }
    }
}

// ---- battery ----------------------------------------------------------------------------------------
// s_bat: 0 nothing in the left hand, 1 a NEW battery, 2 gripping the old one in the camcorder (not out
// yet), 3 holding the old one (pulled out). The old battery's removal is only shown: the game's own
// battery keeps running until the new one goes in and its reload replaces it - so pulling one out and
// walking off can never leave you without a camcorder light.
static int s_bat = 0;
static bool s_slotLoaded = true;
static ULONGLONG s_emptyFlashUntil = 0, s_reloadFlashUntil = 0, s_chargedUntil = 0, s_ejectFirstUntil = 0;
static V3 s_pouch, s_slot;
static bool s_slotValid = false;
// The battery sits in the back of the camcorder: 7 cm behind and 3 cm above the right controller's grip.
static const V3 kSlotOff = { 0.0f, 0.03f, 0.07f };
static void BatteryInsert(ULONGLONG now, float d) {
    s_reloadUntil = now + 150; s_reloadFlashUntil = now + 600; s_chargedUntil = now + 1100;
    s_gesturePauseUntil = now + 2500;   // let the game's reload play out before any gesture
    s_rearmAfterPause = (s_camUp == 1);
    OLXR_HapticPulse(0, 0.7f, 50); OLXR_HapticPulse(1, 0.9f, 70);
    ILog("[OLVR][INTERACT] battery inserted at the camcorder (%.2f m) -> reload", d);
}
static void Battery(ULONGLONG now, const OLXrFrameHands& fh, const OLXrPad& p) {
    s_pouch = OnBody(-0.20f, -0.58f, -0.04f);
    const bool lOk = (fh.valid & 1u) != 0, rOk = (fh.valid & 2u) != 0;
    const V3 L = P3(fh.hand[0]), R = P3(fh.hand[1]);
    s_inPouch = lOk && Len(Sub(L, s_pouch)) < 0.18f;
    s_moving = sqrtf(p.moveX * p.moveX + p.moveY * p.moveY) > 0.35f;
    s_slotValid = rOk && HolsterOn() && s_hState == H_IN_HAND;
    if (rOk) s_slot = Add(R, QRot(fh.hand[1], kSlotOff));
    // A reload the game did by itself (Y) always leaves a loaded camcorder.
    if (s_prevNumBat >= 0 && s_numBat < s_prevNumBat) { s_slotLoaded = true; if (s_bat == 1) s_bat = 0; }
    s_prevNumBat = s_numBat;
    if (!g_on[ON_BATTERY]) { s_bat = 0; return; }

    if (!HolsterOn()) {
        // Test37: battery from the hip, let go at the camcorder hand.
        if (s_bat == 0) {
            if (s_gripEdge[0] && s_inPouch && !s_moving) {
                if (s_numBat > 0) { s_bat = 1; OLXR_HapticPulse(0, 0.6f, 45); ILog("[OLVR][INTERACT] battery taken from the hip pouch (%d left)", s_numBat); }
                else { s_emptyFlashUntil = now + 700; OLXR_HapticPulse(0, 0.9f, 25); ILog("[OLVR][INTERACT] hip pouch is EMPTY (no spare batteries)"); }
            }
        } else if (!s_gripDown[0]) {
            const float d = (lOk && rOk) ? Len(Sub(L, R)) : 9.0f;
            if (d < 0.18f) BatteryInsert(now, d);
            else ILog("[OLVR][INTERACT] battery let go away from the camcorder (%.2f m) - put back", d);
            s_bat = 0;
        }
        return;
    }

    // Test38: eject, drop, new one from the hip, slide it in.
    const float dSlot = (lOk && s_slotValid) ? Len(Sub(L, s_slot)) : 9.0f;
    switch (s_bat) {
    case 0:
        if (!s_gripEdge[0]) break;
        if (s_slotLoaded && dSlot < 0.09f) {
            if (s_numBat > 0) {
                s_bat = 2; OLXR_HapticPulse(0, 0.45f, 30);
                ILog("[OLVR][RELOAD] old battery gripped at the camcorder (%.2f m) - pull it out", dSlot);
            } else {
                s_emptyFlashUntil = now + 900; OLXR_HapticPulse(0, 0.9f, 25);
                ILog("[OLVR][RELOAD] no spare batteries - the old one stays in");
            }
        } else if (s_inPouch && !s_moving) {
            if (s_slotLoaded) {
                s_ejectFirstUntil = now + 2000; OLXR_HapticPulse(0, 0.2f, 15);   // first the old one out
                ILog("[OLVR][RELOAD] hip pouch: the old battery has to come out first");
            } else if (s_numBat > 0) {
                s_bat = 1; OLXR_HapticPulse(0, 0.6f, 45);
                ILog("[OLVR][RELOAD] new battery taken from the hip pouch (%d left)", s_numBat);
            } else {
                s_emptyFlashUntil = now + 900; OLXR_HapticPulse(0, 0.9f, 25);
                ILog("[OLVR][RELOAD] hip pouch is EMPTY (no spare batteries)");
            }
        }
        break;
    case 2:
        if (!s_gripDown[0] || !s_slotValid) { s_bat = 0; break; }        // let go before it came out
        if (dSlot > 0.08f) {
            s_bat = 3; s_slotLoaded = false;
            OLXR_HapticPulse(0, 0.8f, 40); OLXR_HapticPulse(1, 0.4f, 25);
            Learn(LN_EJECT);
            ILog("[OLVR][RELOAD] old battery pulled out of the camcorder");
        }
        break;
    case 3:
        if (!s_gripDown[0]) {
            if (s_slotValid && dSlot < 0.07f) {
                s_slotLoaded = true; OLXR_HapticPulse(0, 0.5f, 30);
                ILog("[OLVR][RELOAD] the old battery went back in");
            } else {
                OLXR_HapticPulse(0, 0.3f, 25);
                ILog("[OLVR][RELOAD] old battery dropped");
            }
            s_bat = 0;
        }
        break;
    case 1:
        if (s_slotValid && dSlot < 0.07f) {
            s_bat = 0; s_slotLoaded = true;
            Learn(LN_INSERT);
            BatteryInsert(now, dSlot);
        } else if (!s_gripDown[0]) {
            s_bat = 0;
            ILog("[OLVR][RELOAD] new battery let go away from the camcorder - back in the pouch");
        }
        break;
    }
}

// ---- doors (Test40: [DOORPHYS] a hinged body with weight) --------------------------------------------
// Test39 wrote TargetOpenRatio and let OLDoor's tick chase it at a constant CurrentSpeed: a robot swing
// that lagged a fast hand, stopped dead, and jumped when a hand started past the panel's edge. Now a
// door a hand touches is a heavy hinged body - an angle, an angular speed and hinge friction:
//  - push: the panel is a contact. It cannot move away faster than its weight lets your hand speed it
//    up (Miles's hand stops at its surface meanwhile), and when your hand stops it keeps its speed and
//    slows down by itself: a shove swings it on a little, a slow push creeps it open.
//  - handle: a stiff spring between the handle and your hand. The door lags a touch behind a fast pull
//    and swings on when you let go mid-swing. Miles's hand stays on the handle.
//  - it stops against Miles instead of passing through him, bounces softly off its open stop and
//    latches when it swings shut.
// The angle goes in through the game's own set-ratio (0xCC46A0: angle, target and the AI's navmesh
// obstacle together - OLDoor's procedural swings call it every tick), so its tick has nothing left to
// chase. Whatever the game does with a door itself - X, an AI going through, a script, a locked
// rattle - takes it straight back.
struct NearDoor { void* d; V3 knob, hinge; float ratio, maxAng; bool locked, rev, cant, busy; float dist; unsigned char mat; };
static void* s_all[512]; static int s_allN = 0; static ULONGLONG s_allMs = 0;
static NearDoor s_near[12]; static int s_nearN = 0;
static V3 s_handW[2]; static bool s_handOk[2] = { false, false };   // this tick's hands in the world
struct Grab { void* d; float lastR; float yaw; };   // (Beta 1.1) yaw: the body's facing when it took hold
static Grab s_grab[2] = {};
static ULONGLONG s_lockedFlashUntil = 0; static void* s_lockedDoor = nullptr;
static const float kGrabUU = 26.0f, kSlipUU = 64.0f, kContactUU = 7.0f;  // the hand must be at the handle
static const float kDoorWideUU = 80.0f;   // near enough that the right grip belongs to the door, not the camcorder
// (Beta 1.1) [DOORLETGO] ... the handle is also let go once the body has turned away, the knob is out of reach, or behind Miles.
static const float kLetGoTurnDeg = 45.0f, kLetGoReachUU = 95.0f;
static const float kPushAccel = 1600.0f;                 // deg/s^2 a hand can give a heavy door
static const float kFricVisc = 1.1f, kFricCoul = 22.0f;  // hinge friction: lighter door, easier to move
static const float kHeroRadius = 26.0f;                  // Miles, for the panel to stop against
// [DOOR] hold-and-pull tuning. The handle no longer copies the hand's ANGLE around the hinge: the hand's
// own travel along the knob's opening tangent is scaled into door degrees. That gives the arm its
// amplitude (a 50 cm pull can swing a 95 deg door all the way) and the same pull always gives the same
// rotation, wherever the hand is around the hinge. The spring is deliberately soft: the door follows the
// hand's acceleration and speed instead of snapping to the target, so a tiny pull no longer flings it.
static const float kGrabDeadzone = 6.0f;     // small hand wiggles do not move the door (mask bad placements)
static const float kHandleGain = 1.7f;       // door degrees per uu of hand travel along the opening tangent
static const float kHandleK = 110.0f, kHandleC = 18.0f;  // how softly the door follows that target
static const float kHandleVelGain = 0.8f;    // deg/s^2 per (uu/s) of hand speed (angular acceleration with hand speed)
static const float kHandleMaxOm = 150.0f;    // deg/s the hand can swing it at most (no zip)
static const float kReleaseTravel = 60.0f;   // hand travel from the grab point before it lets go (cm)
static const float kAutoOpenRatio = 0.70f;   // let go past this (still opening) -> the door finishes opening
static const float kAutoCloseMove = 24.0f;   // pulled/pushed this far toward closing at let-go -> it closes by itself
static const float kOpenSwing = 60.0f;       // deg/s used to finish an auto-open / auto-close
static const float kHandleFaceUU = 6.0f;     // handle-to-handle depth of a door (the second knob is on the other face)
static const float kSndMove = 0.15f;         // door travel (ratio) before a movement sound starts (sooner = faster feedback)
static const float kSndSpeed = 25.0f;        // deg/s; slower than this and it is only a nudge / a held door
static const ULONGLONG kSndBlockMs = 400;    // no new movement sound for this long after one (faster repeat)
static const ULONGLONG kSndCloseCutMs = 900; // keep only the start of the closing sound (harsh latch tail cut more)
// [DOOR] max arm reach: once the hand is this far from the grab point the door no longer follows it
// (the arm is fully extended, so the door stops moving instead of the hand staying behind).
static const float kMaxArmReachUU = 95.0f;   // cm beyond which the door stops following the hand
// [DOOR] how far in front of the panel the open hand's anchor sits, so the fingers (which extend past
// the wrist) stop on the door surface instead of sinking through it.
static const float kHandDepthUU = 11.0f;
// [DOOR] after letting go of a held door, keep that hand out of the push loop for a moment so the
// door can swing through it (it was just keeping the door from finishing its travel).
static const ULONGLONG kPushSuppressMs = 2500;
// [DOORPUSH] which side of which door each open hand is on (0 = not on any panel)
static void* s_pushDoor[2] = { nullptr, nullptr };
static int   s_pushSide[2] = { 0, 0 };
static ULONGLONG s_pushLockedMs = 0;
static ULONGLONG s_pushSuppressUntil[2] = { 0, 0 };   // [DOOR] no push from a hand that just let go of a door
static float s_gripW[2] = { 0.0f, 0.0f };     // [DOORGRIP] eased
struct DoorSim { void* d; float om, lastCmd, lastHapR; bool live; bool pushed; ULONGLONG ms; float startR; bool evOpen, sndOpened, sndClosed; bool autoRun; float autoTo; ULONGLONG gameUntil; int sndDir; float sndFrom; ULONGLONG sndBlockUntil, sndCloseCut; };
// [DOOREVENT] (Test43) What the game's own door code does as a door opens and closes (OLGame.exe):
//  open  (AOLDoor 0xD4A770, the door's "open" for X, AI and scripts): if DoorState is 0 (shut) it fires
//        TriggerEvent(0), sets DoorState 1, and fires TriggerEvent(1).
//  close (0xD58AEB): TriggerEvent(2), DoorState 2 until it is shut again (0).
// TriggerEvent(n) activates output n of the door's OLSeqEvent_Door Kismet events - what the level
// scripts' scares listen to. The hand physics moved the door straight through 0xCC46A0 and did none of
// it: the first scare at a door never came. Now a door the hands open from shut, and one they swing
// shut, go through the same steps.
static bool WrB(void* base, unsigned off, unsigned char v) {
    if (!base || !off) return false;
    __try { *(volatile unsigned char*)((char*)base + off) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static DoorSim s_sim[6] = {};
static V3 s_doorAnchor[2]; static bool s_doorAnchorOn[2] = { false, false };
static volatile long s_devDoorEv = -1;
// [DOOR] a hand that grabs the handle is anchored at a fixed world point so Miles's hand never wanders into
// the door or between two knobs; small wiggles inside the deadzone are masked.
static V3   s_grabFixPt[2] = {{0,0,0},{0,0,0}};   // world point the hand is pinned to while holding the knob
static bool  s_grabFixed[2] = {false,false};      // true while the current grab uses a fixed anchor
static V3    s_grabTan[2] = {{0,0,0},{0,0,0}};    // world unit along which the knob moves as the door opens
static float s_grabA0[2] = {0.0f,0.0f};           // door angle at the grab (deg)
static float s_grabS[2] = {0.0f,0.0f};            // hand travel along that tangent since the grab (uu, + opens)
static float s_grabSPrev[2] = {0.0f,0.0f};
static int   s_grabSide[2] = {1,1};               // which face of the door the palm is on
static float s_anchorYaw[2] = {0.0f,0.0f};        // door swing since the grab, for the locked wrist
static bool  s_anchorLock[2] = {false,false};     // true while the wrist is pinned to the door, not the controller
static ULONGLONG s_grabHapUntil[2] = {0,0};       // light buzz while a hand is holding a knob
extern "C" void OLInteract_DevDoorEvent(int type) { InterlockedExchange(&s_devDoorEv, type); }   // [DEVBRIDGE] "doorev"

static bool ReadDoor(void* d, NearDoor* o) {
    unsigned char type = 0; uint32_t bits = 0;
    if (!Rd(d, F.dtype, &type) || type > 2) return false;             // 0 wood / 1 metal / 2 security; lockers etc. stay native
    if (!Rd(d, F.bits, &bits) || !Rd(d, F.openR, &o->ratio) || !Rd(d, F.maxAng, &o->maxAng)) return false;
    o->mat = type;
    float k[3], h[3];
    if (!OLUE3_DoorKnobWorld(d, k) || !OLUE3_DoorHinge(d, h)) return false;
    o->d = d; o->knob = { k[0], k[1], k[2] }; o->hinge = { h[0], h[1], h[2] };
    o->locked = (bits & F.mLocked) && !(F.mFake && (bits & F.mFake));
    o->rev = F.mRev && (bits & F.mRev); o->cant = F.mCant && (bits & F.mCant);
    if (!(o->maxAng > 5.0f && o->maxAng < 200.0f)) o->maxAng = 95.0f;
    // the game is doing something with it: a procedural rattle / blocked / auto-close swing, an AI
    // going through, its own open-close animation, broken
    unsigned char st = 0, brk = 0; void* user = nullptr;
    o->busy = (F.mBusyBits && (bits & F.mBusyBits)) ||
              (F.dstate && Rd(d, F.dstate, &st) && (st == 3 || st == 4)) ||
              (F.dbreak && Rd(d, F.dbreak, &brk) && brk != 0) ||
              (F.duser && Rd(d, F.duser, &user) && user && user != OLUE3_Hero());
    return true;
}
static float PanelDist(V3 hinge, float phiDeg, float width, V3 p) {
    const float c = cosf(phiDeg * 0.0174532925f), s = sinf(phiDeg * 0.0174532925f);
    const float px = p.x - hinge.x, py = p.y - hinge.y;
    float t = px * c + py * s; if (t < 0.0f) t = 0.0f; if (t > width) t = width;
    const float dx = px - c * t, dy = py - s * t;
    return sqrtf(dx * dx + dy * dy);
}
static DoorSim* SimFor(void* d, bool create) {
    for (DoorSim& s : s_sim) if (s.d == d) return &s;
    if (!create) return nullptr;
    for (DoorSim& s : s_sim) if (!s.live) { s = {}; s.d = d; return &s; }
    return nullptr;
}
static bool ClimbHolding(int h);   // [CLIMB] below: a hand that holds an edge is not a door hand
static float BodyYaw();             // (Beta 1.1) [DOORLETGO] below
static bool CamInRightHand();
static void Doors(ULONGLONG now, float dt, V3 handW[2], bool handOk[2]) {
    s_doorAnchorOn[0] = s_doorAnchorOn[1] = false;
    if (!g_on[ON_DOORS]) {
        s_grab[0].d = s_grab[1].d = nullptr; s_nearN = 0; s_nearKnob[0] = s_nearKnob[1] = false;
        s_nearDoorWide[0] = s_nearDoorWide[1] = false;
        s_anchorLock[0] = s_anchorLock[1] = false; s_grabFixed[0] = s_grabFixed[1] = false;
        for (DoorSim& s : s_sim) { s.live = false; s.autoRun = false; }
        return;
    }
    // [DOOR] the closing sound is cut after its start (its latch end is harsh); do this for every sim,
    // even one that has already come to rest, so the harsh tail never plays.
    for (DoorSim& s : s_sim) {
        if (s.sndCloseCut && now >= s.sndCloseCut) {
            s.sndCloseCut = 0;
            OLSound_StopByName("EV_Closing_Wood_Door", 0);
        }
    }
    if (now - s_allMs > 2000) { s_allN = OLUE3_ObjectsOfClass("OLDoor", s_all, 512); s_allMs = now; }
    if (s_allN > 0) OLUE3_DoorEventHook(s_all[0]);                       // (Test43) [DOOREVENT] once, from a door's vtable
    // Doors within 4 m, nearest knob first.
    NearDoor cand[32]; int nc = 0;
    for (int i = 0; i < s_allN && nc < 32; ++i) {
        V3 l;
        if (!Rd(s_all[i], F.loc, &l)) continue;
        const float dx = l.x - s_headW.x, dy = l.y - s_headW.y, dz = l.z - s_headW.z;
        if (sqrtf(dx * dx + dy * dy) > 400.0f || fabsf(dz) > 300.0f) continue;
        if (!ReadDoor(s_all[i], &cand[nc])) continue;
        cand[nc].dist = Len(Sub(cand[nc].knob, s_headW));
        ++nc;
    }
    for (int i = 1; i < nc; ++i) {   // insertion sort, nc <= 32
        NearDoor t = cand[i]; int j = i - 1;
        while (j >= 0 && cand[j].dist > t.dist) { cand[j + 1] = cand[j]; --j; }
        cand[j + 1] = t;
    }
    s_nearN = nc < 12 ? nc : 12;
    for (int i = 0; i < s_nearN; ++i) s_near[i] = cand[i];
    {   // [DEVBRIDGE] "doorev": one event at the nearest door
        const long t = InterlockedExchange(&s_devDoorEv, -1);
        if (t >= 0 && s_nearN > 0) ILog("[OLVR][DOOREVENT] (dev) event %ld at the nearest door: %s", t, OLUE3_DoorTriggerEvent(s_near[0].d, (int)t) ? "fired" : "FAILED");
    }
    // a sim whose door left the list (out of range, gone) stops
    for (DoorSim& s : s_sim) {
        if (!s.live) continue;
        bool there = false;
        for (int i = 0; i < s_nearN; ++i) if (s_near[i].d == s.d) there = true;
        if (!there) s.live = false;
    }
    V3 hero = { 0, 0, 0 }; const bool heroOk = Rd(OLUE3_Hero(), F.loc, &hero);
    int dlm = 0, dsm = 0; OLUE3_HeroMoveState(&dlm, &dsm);
    const bool heroDoorMove = (dlm == 2 && dsm >= 28 && dsm <= 36) || dlm == 7;   // (Test43) his own door moves (X)
    // ---- the hands: who holds which handle, who could squeeze one -------------------------------------
    int best[2] = { -1, -1 }; float bestD[2] = { 1e9f, 1e9f };
    for (int h = 0; h < 2; ++h) {
        s_nearKnob[h] = false;
        Grab& g = s_grab[h];
        // [DOOR] light buzz while a hand holds a knob
        if (s_grabFixed[h] && now > s_grabHapUntil[h]) { s_grabHapUntil[h] = now + 350; OLXR_HapticPulse(h, 0.25f, 15); }
        if (!handOk[h] || ClimbHolding(h) || (h == 1 && (CamInRightHand() || g_camButton || OLInteract_FaceCamPinned()))) {
            s_grabFixed[h] = false; s_anchorLock[h] = false; g.d = nullptr;
            s_nearDoorWide[h] = false;
            continue;
        }
        for (int i = 0; i < s_nearN; ++i) {
            const float d = Len(Sub(handW[h], s_near[i].knob));
            if (d < bestD[h]) { bestD[h] = d; best[h] = i; }
        }
        s_nearKnob[h] = best[h] >= 0 && bestD[h] < kGrabUU;
        s_nearDoorWide[h] = best[h] >= 0 && bestD[h] < kDoorWideUU;
        if (g.d) {
            const NearDoor* nd = nullptr;
            for (int i = 0; i < s_nearN; ++i) if (s_near[i].d == g.d) nd = &s_near[i];
            // [DOORLETGO] how far the body has turned since it took hold, and where the knob sits from the head
            float turned = (BodyYaw() - g.yaw) * 57.2957795f;
            while (turned > 180.0f) turned -= 360.0f;
            while (turned < -180.0f) turned += 360.0f;
            float reach = 0.0f;
            if (nd) {
                const float kx = nd->knob.x - s_headW.x, ky = nd->knob.y - s_headW.y;
                reach = sqrtf(kx * kx + ky * ky);
            }
            // the grip lets go only after a real pull away / turn / walk off, not on the small hand movements
            // that are just moving the door.
            const float dx = handW[h].x - s_grabFixPt[h].x, dy = handW[h].y - s_grabFixPt[h].y;
            const float travel = sqrtf(dx * dx + dy * dy);
            const float sNow = dx * s_grabTan[h].x + dy * s_grabTan[h].y;
            const bool turnedAway = fabsf(turned) > kLetGoTurnDeg;
            const bool outOfReach = reach > kLetGoReachUU;
            const bool tooFar = travel > kReleaseTravel;
            if (!s_gripDown[h] || !nd || nd->busy || turnedAway || outOfReach || tooFar) {
                ILog("[OLVR][DOORPHYS] handle let go by the %s hand (%s) at ratio %.2f", h ? "right" : "left",
                     !s_gripDown[h] ? "grip open" : (!nd ? "door gone" : (nd->busy ? "the game took the door" :
                     (turnedAway ? "the body turned away" : (outOfReach ? "out of reach" : "pulled away from the handle")))),
                     nd ? nd->ratio : -1.0f);
                // [DOOR] auto-finish on let-go: an open door released while still opening finishes opening;
                // an open door the hand clearly pushed toward closing is closed on its own. Anything below
                // the opening threshold just stays where it was - releasing a half-open door never slams it.
                if (nd) {
                    DoorSim* ds = SimFor(nd->d, true);
                    if (ds) {
                        bool act = false;
                        if (nd->ratio > kAutoOpenRatio && sNow > -kAutoCloseMove * 0.5f) { ds->autoTo = 1.0f; act = true; }
                        else if (nd->ratio > 0.20f && sNow < -kAutoCloseMove) { ds->autoTo = 0.0f; act = true; }
                        if (act) {
                            ds->live = true; ds->autoRun = true; ds->om = (ds->autoTo > 0.5f ? kOpenSwing : -kOpenSwing);
                            ds->sndDir = 0; ds->sndFrom = nd->ratio; ds->sndBlockUntil = 0; ds->sndCloseCut = 0;
                            ds->lastCmd = nd->ratio; ds->startR = nd->ratio;
                            ds->sndOpened = ds->sndClosed = false;   // let the finish play its sound
                            // [DOOR] sound is posted from the movement branch below (material-aware),
                            // so it is not repeated here.
                            ILog("[OLVR][DOORPHYS] auto-finish door at ratio %.2f (hand travel %+.0f uu) -> %s",
                                 nd->ratio, sNow, ds->autoTo > 0.5f ? "opening" : "closing");
                        }
                    }
                }
                OLXR_HapticPulse(h, 0.3f, 25);
                s_grabFixed[h] = false; s_anchorLock[h] = false; s_grabS[h] = s_grabSPrev[h] = 0.0f;
                s_pushSuppressUntil[h] = now + kPushSuppressMs;   // let the door swing through this hand for a moment
                g.d = nullptr;
            }
            continue;
        }
        if (!s_gripEdge[h] || best[h] < 0 || bestD[h] >= kGrabUU) continue;
        if (h == 0 && s_bat) continue;                                     // the left hand holds a battery
        const NearDoor& nd = s_near[best[h]];
        if (nd.locked) {
            s_lockedFlashUntil = now + 900; s_lockedDoor = nd.d;
            OLXR_HapticPulse(h, 0.9f, 90);
            ILog("[OLVR][INTERACT] door knob grabbed by the %s hand - LOCKED", h ? "right" : "left");
            continue;
        }
        if (nd.busy) continue;
        g.d = nd.d; g.lastR = nd.ratio; g.yaw = BodyYaw();
        // [DOOR] pin Miles's hand at a fixed world point on the knob so it never wanders into the door
        // or between two knobs; small wiggles are masked by the deadzone in the handle spring below.
        const float phi0 = atan2f(nd.knob.y - nd.hinge.y, nd.knob.x - nd.hinge.x);
        const float sign = nd.rev ? 1.0f : -1.0f;
        s_grabFixPt[h] = handW[h];
        s_grabTan[h] = { sign * -sinf(phi0), sign * cosf(phi0), 0.0f };
        s_grabA0[h] = nd.ratio * nd.maxAng;
        s_grabS[h] = s_grabSPrev[h] = 0.0f;
        // which face of the door the palm is on: keep Miles's hand on that face's handle for the whole
        // grab, so the hand always ends up at the same correct spot on the knob - never inside the door
        // or between the two knobs (a door has one handle on each face). The side is fixed at grab time
        // and never revisited while the door swings, so a small hand wiggle cannot flip the hand to the
        // other face. The game's knob sits on one face; the other face's handle is its mirror (kHandleFaceUU).
        // Decided from the HAND's position at grab time, relative to the panel normal.
        const float nx = -sinf(phi0), ny = cosf(phi0);
        const float side = (handW[h].x - nd.hinge.x) * nx + (handW[h].y - nd.hinge.y) * ny;
        s_grabSide[h] = side < 0.0f ? -1 : 1;
        s_anchorYaw[h] = 0.0f; s_anchorLock[h] = true; s_grabFixed[h] = true;
        s_grabHapUntil[h] = now + 250;   // buzz while holding the knob
        DoorSim* ds = SimFor(nd.d, false);
        if (ds) { ds->autoRun = false; ds->sndOpened = ds->sndClosed = false; ds->sndDir = 0; ds->sndFrom = nd.ratio; ds->sndBlockUntil = now; }
        OLXR_HapticPulse(h, 0.5f, 40);
        Learn(LN_DOOR);
        ILog("[OLVR][DOORPHYS] handle grabbed by the %s hand (%.0f uu off) - ratio %.2f, max %.0f deg%s%s",
             h ? "right" : "left", bestD[h], nd.ratio, nd.maxAng, nd.rev ? ", reversed" : "", nd.cant ? ", can't close" : "");
    }
    // ---- each door a hand touches (or that is still swinging) ----------------------------------------
    for (int i = 0; i < s_nearN; ++i) {
        const NearDoor& nd = s_near[i];
        if (heroDoorMove) {
            // (Test43) Miles is opening / closing a door the game's way (X: its door moves 28-36, or the door
            // locomotion): every door near him is the game's until 2 s after - a hand in the swing of a door
            // X opened pushed it back shut
            DoorSim* hs = SimFor(nd.d, nd.dist < 300.0f);
            if (hs) { hs->live = false; hs->gameUntil = now + 2000; }
            for (int h = 0; h < 2; ++h) {
                if (s_grab[h].d == nd.d) { s_grab[h].d = nullptr; s_grabFixed[h] = false; s_anchorLock[h] = false; }
                if (s_pushDoor[h] == nd.d) { s_pushDoor[h] = nullptr; s_pushSide[h] = 0; }
            }
            continue;
        }
        const float ex = nd.knob.x - nd.hinge.x, ey = nd.knob.y - nd.hinge.y;
        const float width = sqrtf(ex * ex + ey * ey) + 8.0f;
        const float phi0 = atan2f(ey, ex) * 57.2957795f;               // the panel, hinge -> knob (deg)
        const float sign = nd.rev ? 1.0f : -1.0f;                      // panel angle per ratio (0xCB2300)
        // open hands against this panel: which side, how far in
        bool contact[2] = { false, false }; float aC[2] = { 0, 0 }, dirA[2] = { 0, 0 };
        for (int h = 0; h < 2; ++h) {
            if (!handOk[h] || s_gripDown[h] || ClimbHolding(h) || (h == 1 && (CamInRightHand() || g_camButton)) || (h == 0 && s_bat)) {
                if (s_pushDoor[h] == nd.d) { s_pushDoor[h] = nullptr; s_pushSide[h] = 0; }
                continue;
            }
            const float hx = handW[h].x - nd.hinge.x, hy = handW[h].y - nd.hinge.y;
            const float r = sqrtf(hx * hx + hy * hy);
            const bool inBand = r >= 10.0f && r <= width && handW[h].z > nd.knob.z - 95.0f && handW[h].z < nd.knob.z + 120.0f;
            const float aHand = atan2f(hy, hx) * 57.2957795f, d = Wrap180(aHand - phi0);
            if (!inBand || fabsf(d) > 60.0f) {                         // not at this panel
                if (s_pushDoor[h] == nd.d) { s_pushDoor[h] = nullptr; s_pushSide[h] = 0; }
                continue;
            }
            const float perp = r * sinf(d * 0.0174532925f);
            int sd = (s_pushDoor[h] == nd.d) ? s_pushSide[h] : 0;
            // the side is decided once, clearly on one side - a fast shove that outruns the panel
            // must not end up "on the other side" and pull the door back as the hand comes home
            if (sd == 0 && fabsf(perp) > kContactUU + 2.0f) sd = perp > 0 ? 1 : -1;
            s_pushDoor[h] = nd.d; s_pushSide[h] = sd;
            if (sd == 0 || perp * sd > kContactUU + 0.5f) continue;     // not touching (or no side yet)
            // the panel angle (in "ratio degrees") where it just touches this hand on its side, and which
            // way is toward the hand: the panel may not pass that angle toward the hand
            const float s = kContactUU / r < 1.0f ? kContactUU / r : 1.0f;
            const float aTouch = aHand - sd * asinf(s) * 57.2957795f;
            contact[h] = true;
            aC[h] = nd.ratio * nd.maxAng + sign * Wrap180(aTouch - phi0);
            dirA[h] = sign * (float)sd;
        }
        int ng = 0; int gh[2];
        for (int h = 0; h < 2; ++h) if (s_grab[h].d == nd.d) gh[ng++] = h;
        DoorSim* s = SimFor(nd.d, false);
        if (!ng && !contact[0] && !contact[1] && !(s && s->live)) continue;
        if (nd.locked) {
            if ((contact[0] || contact[1]) && now - s_pushLockedMs > 1500) {
                s_pushLockedMs = now; s_lockedFlashUntil = now + 900; s_lockedDoor = nd.d;
                OLXR_HapticPulse(contact[1] ? 1 : 0, 0.7f, 60);
                ILog("[OLVR][DOORPUSH] pushed a LOCKED door with the %s hand", contact[1] ? "right" : "left");
            }
            if (s) s->live = false;
            continue;
        }
        if (nd.busy) { if (s && s->live) { s->live = false; ILog("[OLVR][DOORPHYS] the game took a door back (its own swing / an AI / a script)"); } continue; }
        // (Test43) Miles is opening / closing a door the game's way (X: its door moves 28-36, or the door
        // locomotion): every door is the game's until he is done - a hand in the swing pushed one back shut
        if (s && !ng && now < s->gameUntil) {                          // just moved by the game: an open hand in its way waits,
            for (int h = 0; h < 2; ++h) if (s_pushDoor[h] == nd.d) { s_pushDoor[h] = nullptr; s_pushSide[h] = 0; }   // and finds its side afresh
            continue;
        }
        if (!s) s = SimFor(nd.d, true);
        if (!s) continue;
        if (s->live && !ng && !contact[0] && !contact[1] && !s->autoRun && fabsf(nd.ratio - s->lastCmd) > 0.06f) {
            // somebody else moved it (X, a script): it is the game's until it is touched again. While a
            // hand is on it we keep it: the game's own auto-close must not pull it shut under the hand.
            ILog("[OLVR][DOORPHYS] door moved by the game (%.2f, we left it at %.2f) - handing it back", nd.ratio, s->lastCmd);
            s->live = false; s->om = 0.0f; s->autoRun = false; s->gameUntil = now + 1500;
            for (int h = 0; h < 2; ++h) if (s_grab[h].d == nd.d) { s_grab[h].d = nullptr; s_grabFixed[h] = false; s_anchorLock[h] = false; }
            continue;
        }
        if (!s->live) { s->live = true; s->om = 0.0f; s->lastCmd = nd.ratio; s->lastHapR = nd.ratio; s->pushed = false; s->startR = nd.ratio; s->evOpen = false; s->autoRun = false; s->sndOpened = s->sndClosed = false; s->sndDir = 0; s->sndFrom = nd.ratio; s->sndBlockUntil = 0; s->sndCloseCut = 0; }
        const float maxA = nd.maxAng, A = nd.ratio * maxA;
        const float lo = nd.cant ? A : 0.0f, hi = (nd.ratio > 1.0f ? nd.ratio : 1.0f) * maxA;
        float Om = s->om;
        // the handle: the hand's linear travel along the knob's opening tangent is scaled into door degrees
        if (ng) {
            float alpha = 0.0f;
            for (int k = 0; k < ng; ++k) {
                const int h = gh[k];
                const float dx = handW[h].x - s_grabFixPt[h].x, dy = handW[h].y - s_grabFixPt[h].y;
                float sTravel = dx * s_grabTan[h].x + dy * s_grabTan[h].y;
                if (fabsf(sTravel) <= kGrabDeadzone) sTravel = 0.0f;
                else sTravel -= (sTravel > 0.0f ? kGrabDeadzone : -kGrabDeadzone);
                // [DOOR] arm fully extended: once the hand is beyond kMaxArmReachUU from the grab
                // point the door stops following it (the arm can't reach further).
                const bool armExtended = fabsf(sTravel) > kMaxArmReachUU;
                const float vTravel = (sTravel - s_grabSPrev[h]) / dt;
                s_grabS[h] = sTravel; s_grabSPrev[h] = sTravel;
                float target = s_grabA0[h] + kHandleGain * sTravel;
                if (target < lo) target = lo; if (target > hi) target = hi;
                if (!armExtended) {
                    alpha += kHandleK * (target - A) - kHandleC * Om + kHandleVelGain * vTravel;
                }
            }
            alpha /= (float)ng;
            if (alpha > 2500.0f) alpha = 2500.0f; if (alpha < -2500.0f) alpha = -2500.0f;
            Om += alpha * dt;
            if (Om > kHandleMaxOm) Om = kHandleMaxOm; if (Om < -kHandleMaxOm) Om = -kHandleMaxOm;
        } else if (s->autoRun) {
            // a door released past the opening threshold finishes on its own; one pushed toward closing
            // while open closes on its own. It drives all the way instead of coasting to a stop.
            const float want = s->autoTo * maxA;
            const float diff = want - A;
            if (fabsf(diff) < 0.75f) { Om = 0.0f; s->autoRun = false; }
            else Om = (diff > 0.0f ? 1.0f : -1.0f) * kOpenSwing;
        } else {   // hinge friction
            const float dec = kFricCoul * dt;
            Om = fabsf(Om) <= dec ? 0.0f : Om - (Om > 0.0f ? dec : -dec);
            Om *= expf(-kFricVisc * dt);
        }
        float An = A + Om * dt;
        // pushing hands: an open hand pushes the panel, but a hand that just let go of a held door is
        // suppressed for a moment (kPushSuppressMs) so the door can swing through it and finish its
        // travel instead of being stopped right after release.
        bool anyPush = false;
        for (int h = 0; h < 2; ++h) {
            if (!contact[h] || now < s_pushSuppressUntil[h]) continue;
            anyPush = true;
            const float need = (aC[h] - A) / dt, maxDv = kPushAccel * dt;
            float dv = need - Om; if (dv > maxDv) dv = maxDv; if (dv < -maxDv) dv = -maxDv;
            if (dirA[h] * dv < 0.0f) Om += dv;                         // push the door away from the hand
            An = A + Om * dt;
        }
        if (anyPush) { s->pushed = true; s->autoRun = false; }
        else if (s->pushed) s->pushed = false;
        // stops: open (a soft bounce), closed (it latches), can't-close doors stay where they are
        if (An > hi) {
            An = hi;
            const bool justOpened = Om > 0.0f;
            if (justOpened) { if (Om > 60.0f) for (int h = 0; h < 2; ++h) if (contact[h] || s_grab[h].d == nd.d) OLXR_HapticPulse(h, 0.35f, 30); Om = -0.25f * Om; }
        }
        if (An < lo) {
            An = lo;
            if (Om < 0.0f) {
                if (Om < -60.0f) {
                    ILog("[OLVR][DOORPHYS] the door swung shut (%.0f deg/s)", -Om);
                    for (int h = 0; h < 2; ++h) if (handOk[h] && Len(Sub(handW[h], nd.knob)) < 80.0f) OLXR_HapticPulse(h, 0.6f, 45);
                }
                Om = 0.0f;
            }
        }
        // [DOOR] movement sound: a material-aware event (EV_Opening_Wood/Metal/Security_Door) plays
        // when the door starts a new swing and has moved far enough to be a real open/close - not for
        // a nudge or a held door. It plays once per swing in each direction.
        if (fabsf(Om) <= kSndSpeed) s->sndDir = 0;   // at rest: the next movement is a new swing
        if ((ng || anyPush || s->autoRun) && fabsf(Om) > kSndSpeed) {
            const float sndRatio = An / maxA;
            const int dir = Om > 0.0f ? 1 : -1;
            if (dir != s->sndDir) { s->sndDir = dir; s->sndFrom = sndRatio; }
            if (now >= s->sndBlockUntil && fabsf(sndRatio - s->sndFrom) > kSndMove) {
                static const char* const kMatSnd[3] = {
                    "EV_Opening_Wood_Door", "EV_Opening_Metal_Door", "EV_Opening_Security_Door" };
                const int mi = (nd.mat <= 2) ? nd.mat : 0;
                OLSound_PostByName(kMatSnd[mi], 0);
                s->sndBlockUntil = now + kSndBlockMs;
                s->sndFrom = sndRatio;
                if (dir > 0) { s->sndOpened = true; s->sndClosed = false; }
                else { s->sndClosed = true; s->sndOpened = false; s->sndCloseCut = now + kSndCloseCutMs; }
            }
        }

        // open hands stay in front of the panel: the anchor sits kHandDepthUU in front of the
        // surface so the fingers (which extend past the wrist) stop on the door, not inside it.
        // Applied as soon as the hand is near the panel (not only on contact), so the fingers
        // never sink through while the hand approaches.
        for (int h = 0; h < 2; ++h) {
            if (!handOk[h] || s_gripDown[h] || s_grab[h].d || ClimbHolding(h) ||
                (h == 1 && (CamInRightHand() || g_camButton)) || (h == 0 && s_bat)) continue;
            const float aPanel = (phi0 + sign * (An - A)) * 0.0174532925f;
            const float c = cosf(aPanel), sn = sinf(aPanel);
            const float hx = handW[h].x - nd.hinge.x, hy = handW[h].y - nd.hinge.y;
            const float perp = hx * (-sn) + hy * c;            // signed distance from the panel plane
            if (fabsf(perp) > kHandDepthUU + 10.0f) continue;  // not near this panel
            const float t = hx * c + hy * sn;
            if (t < 0.0f || t > width) continue;               // beyond the panel's edge
            const int sd = perp < 0.0f ? -1 : 1;               // the side the hand is on
            s_doorAnchor[h] = { nd.hinge.x + c * t - sn * sd * kHandDepthUU,
                                nd.hinge.y + sn * t + c * sd * kHandDepthUU,
                                handW[h].z };
            s_doorAnchorOn[h] = true;
        }
        // Miles: the panel now passes through him in both directions - opening *and* closing - so the
        // door never bounces back on Miles when you try to open or close it.
        (void)heroOk; (void)hero;
        const float tn = An / maxA;
        // [DOOREVENT] (Test43) a door the hands open from shut tells the game, as its own opening does
        unsigned char dst = 0xFF;
        if (!s->evOpen && (ng) && s->startR < 0.05f && tn > s->startR + 0.03f) {
            s->evOpen = true;
            if (F.dstate && Rd(nd.d, F.dstate, &dst) && dst != 3 && dst != 4) {
                int fired = 0;
                if (dst == 0) fired += OLUE3_DoorTriggerEvent(nd.d, 0);
                WrB(nd.d, F.dstate, 1);
                fired += OLUE3_DoorTriggerEvent(nd.d, 1);
                ILog("[OLVR][DOOREVENT] the hands opened a door (state %u -> 1): %s", (unsigned)dst, fired ? "the game's open events fired" : "events FAILED");
            }
        }
        // ... and one they swing shut, as its own closing does
        if (tn <= 0.0005f && nd.ratio > 0.01f && (ng || fabsf(Om) > 1.0f || s->om != 0.0f) &&
            F.dstate && Rd(nd.d, F.dstate, &dst) && (dst == 1 || dst == 2)) {
            OLUE3_DoorTriggerEvent(nd.d, 2);
            WrB(nd.d, F.dstate, 0);
            s->evOpen = false; s->startR = 0.0f;
            ILog("[OLVR][DOOREVENT] the hands shut a door: the game's close event fired, state -> 0");
        }
        if (fabsf(tn - nd.ratio) > 1e-5f) {
            if (!OLUE3_DoorSetRatioAndTarget(nd.d, tn)) WrF(nd.d, F.tgtR, tn);
            if (fabsf(tn - s->lastHapR) > 0.025f) {                     // the hinge "creak"
                for (int k = 0; k < ng; ++k) OLXR_HapticPulse(gh[k], 0.12f, 18);
                s->lastHapR = tn;
                if (ng) Learn(LN_DOOR);
            }
            static ULONGLONG s_logMs = 0;
            if ((ng) && now - s_logMs > 1500) {
                s_logMs = now;
                ILog("[OLVR][DOORPHYS] %s: ratio %.2f -> %.2f at %.0f deg/s", ng ? "handle" : "push", nd.ratio, tn, Om);
            }
        }
        s->lastCmd = tn; s->om = Om; s->ms = now;
        // [DOOR] magnet: Miles's hand is anchored to the knob of the face he grabbed, so it is pulled to
        // the correct spot on the handle and follows the door as it swings. This keeps the hand from
        // drifting away or ending up inside the door / between two knobs when you walk around.
        for (int k = 0; k < ng; ++k) {
            const int h = gh[k];
            const float da = sign * (An - A) * 0.0174532925f, c = cosf(da), sn = sinf(da);
            const float kx = nd.knob.x - nd.hinge.x, ky = nd.knob.y - nd.hinge.y;
            const float nkx = kx * c - ky * sn, nky = kx * sn + ky * c;      // the knob after this swing
            const float phiN = phi0 * 0.0174532925f + da;
            const float px = -sinf(phiN), py = cosf(phiN);                   // the panel's normal
            const float dNative = nkx * px + nky * py;                       // how far the game's knob sits off the panel
            const int nativeSide = dNative > 1.0f ? 1 : (dNative < -1.0f ? -1 : 0);
            float off = fabsf(dNative);
            if (nativeSide == 0) off = kHandleFaceUU;                        // knob on the panel plane: use the door depth
            else if (s_grabSide[h] != nativeSide) off += kHandleFaceUU;      // the handle on the other face
            const float ipx = nd.hinge.x + nkx - px * dNative;               // the knob projected onto the panel
            const float ipy = nd.hinge.y + nky - py * dNative;
            s_doorAnchor[h] = { ipx + px * s_grabSide[h] * off,
                                ipy + py * s_grabSide[h] * off,
                                nd.knob.z };
            s_doorAnchorOn[h] = true;
            s_anchorYaw[h] = sign * (An - s_grabA0[h]);   // total swing since the grab: the wrist turns with the door
        }
        if (!ng && !contact[0] && !contact[1] && fabsf(Om) < 1.5f) s->live = false;   // at rest: the game's again
    }
}

// ---- climbing (Test40: [CLIMB]) ---------------------------------------------------------------------
// [Motion] ClimbWithHands=1. Outlast marks every edge you can climb with OLLedgeMarker chains (a marker
// and its Next make one straight edge; its flags say climb up / hang). Squeeze a grip with your hand on
// a marked edge and Miles's hand holds it - it stays there while your real hand moves on. With both
// hands holding, pull down (or toward you) and the game's own climb runs: the mod presses A, exactly
// what its button players do, so the move that fits (vault, climb up, jump and grab) is the game's
// choice. While Miles climbs, a hand that holds on plays his own climbing arm (on the edge); a free
// hand stays yours. Hanging from a ledge: squeeze both grips to hold on (his hands on the edge), pull
// down to climb up, let go of both to drop (B). On a ladder each squeeze takes the rung in front of
// that hand; pull it down and Miles climbs until your hand is back at the rung (the stick still works).
// Reaching up at an edge above your reach and pulling makes him jump for it.
// [Motion] ClimbWithHands=0: the old way - A climbs, the game's arms throughout.
struct Ledge { V3 a, b; bool up; };   // up: bCanClimbUp (else hang only)
static Ledge s_ledges[64]; static int s_ledgeN = 0;
static void* s_lmk[1024]; static int s_lmkN = 0; static ULONGLONG s_lmkMs = 0, s_ledgeMs = 0;
enum { CL_IDLE, CL_PULLING, CL_CLIMBING, CL_HANG, CL_LADDER };
static const char* const kClName[] = { "on the ground", "pull sent (A)", "climbing", "hanging from a ledge", "on a ladder" };
static int s_cl = CL_IDLE;
static bool s_hold[2] = { false, false };       // the hand holds on: an edge, the ledge it hangs from, a rung
static V3 s_holdW[2];                            // where (world, uu)
static V3 s_holdN[2];                            // [edge] horizontal, from the edge toward you
static float s_holdRefY[2] = { 0, 0 };          // [hang] the real hand's height when both took hold (m, from the head)
static int s_ladderDriver = -1;
static bool s_hangHeld = false, s_bothHeldPrev = false, s_pullArmed = true;
static ULONGLONG s_clMs = 0, s_aUntil = 0, s_aFrom = 0, s_bUntil = 0, s_bothOffMs = 0;
static float s_ladderStick = 0.0f; static bool s_ladderStickOn = false;
// (Test40) a pull walks Miles into the edge for a moment before the A: the game only climbs what is
// right in front of him (the first pulls in the headset made him hop 30-40 cm short of it)
static ULONGLONG s_stepUntil = 0; static float s_stepX = 0.0f, s_stepY = 0.0f;
static bool s_stepping = false; static int s_stepHand = 0;
// (Test41) [CLIMBPACE] the climb plays only as fast as the holding hands pull (Boneworks-ish): its
// animation speed follows the hands' downward speed (PaceNominal = the game's own speed, up to 1.8x),
// so one ~45 cm pull carries the whole move; stop pulling and Miles hangs there; let go and the game
// finishes the climb.
static float s_paceRate = 1.0f, s_paceWritten = 1.0f, s_prevRelY[2] = { 0.0f, 0.0f };
// (Test46) the climb under way was started by your hands (a pull sent A) - only such a climb is paced by
// your pull. A jump or vault you press A for yourself plays at the game's speed: with a grip held (the left
// grip is sprint) a sprint-vault was "paced" by hands that were not pulling and froze mid-vault -
// "you cannot vault over objects".
static bool s_climbByHands = false;
static bool s_prevRelOk = false, s_paceGaveUp = false;
static ULONGLONG s_pausedSince = 0;
// (Test41) [LADDERGRAB] walking into a ladder no longer climbs it: the stick stops short of it until a
// hand takes a rung, then Miles steps on as you pull (the game's own mount, paced by your hands).
struct Ladder { V3 bot, top, n; };                 // n: horizontal, from the ladder to where you stand at its foot
static Ladder s_ladders[16]; static int s_ladderN = 0;
static void* s_lad[256]; static int s_ladN = 0; static ULONGLONG s_ladMs = 0, s_ladderMs = 0;
static bool s_holdLadder[2] = { false, false };
static bool s_blockOn = false; static float s_blockX = 0.0f, s_blockY = 0.0f;   // stick frame
// The game mounts a ladder by itself from ~90 uu (measured, walking in: blocking from 90 never caught
// it); the stick stops at 100, where a hand still reaches the rungs. (dev: "ladderblock")
// (Test45) 0: no barrier - "you can't walk directly up close to the ladder to climb it, it's like there is a
// barrier there". Walk up to it and take a rung, or walk into it and the game climbs it its own way.
static float s_ladderBlockUU = 0.0f;
extern "C" void OLInteract_DevLadderBlock(float uu) { s_ladderBlockUU = uu; ILog("[OLVR][LADDERGRAB] (dev) walk stops %.0f uu from a ladder", uu); }
static ULONGLONG s_ladderTakeUntil = 0;
static int s_lm = 0, s_sm = 0;
static V3 s_edgeNear; static bool s_edgeNearOn = false;   // [HOWTO] the edge point a free hand is near
static bool ClimbHolding(int h) { return g_on[ON_CLIMB] && s_hold[h]; }
static volatile long s_fakeReq = 0;            // [DEVBRIDGE] "fakeledge [cm]": an edge 55 cm ahead, 45 cm below the eyes (or cm above them)
static volatile float s_fakeUp = 0.0f;
static Ledge s_fake; static ULONGLONG s_fakeUntil = 0;
extern "C" void OLInteract_DevFakeLedge(float upCm) { s_fakeUp = upCm; InterlockedExchange(&s_fakeReq, 1); }
static bool IsClimbSM(int sm) {
    switch (sm) {
    case 6: case 8: case 9: case 10: case 14: case 15: case 16: case 17: case 18: case 19:
    case 43: case 44: case 45: case 46: case 47: case 48: return true;
    }
    return false;
}
static bool Finite3(V3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && fabsf(v.x) < 1e6f && fabsf(v.y) < 1e6f && fabsf(v.z) < 1e6f; }
static void ClearHolds() { s_hold[0] = s_hold[1] = false; s_holdLadder[0] = s_holdLadder[1] = false; s_ladderDriver = -1; s_ladderStickOn = false; s_hangHeld = false; s_bothHeldPrev = false; }
static void RefreshLedges(ULONGLONG now) {
    if (!F.lmNext) return;
    if (now - s_lmkMs > 4000) { s_lmkN = OLUE3_ObjectsOfClass("OLLedgeMarker", s_lmk, 1024); s_lmkMs = now; }
    if (now - s_ledgeMs < 400) return;
    s_ledgeMs = now; s_ledgeN = 0;
    for (int i = 0; i < s_lmkN && s_ledgeN < 64; ++i) {
        void* m = s_lmk[i]; V3 a, b; void* nx = nullptr;
        if (!Rd(m, F.loc, &a) || !Finite3(a) || fabsf(a.x - s_headW.x) > 600.0f || fabsf(a.y - s_headW.y) > 600.0f || fabsf(a.z - s_headW.z) > 400.0f) continue;
        if (!Rd(m, F.lmNext, &nx) || !nx || !Rd(nx, F.loc, &b) || !Finite3(b) || Len(Sub(a, b)) > 3000.0f) continue;
        uint32_t bits = 0;
        if (F.lmBits && (F.mClimbUp | F.mHang) && Rd(m, F.lmBits, &bits) && !(bits & (F.mClimbUp | F.mHang))) continue;
        uint32_t en = 0;
        if (F.gmBits && F.mEnabled && Rd(m, F.gmBits, &en) && !(en & F.mEnabled)) continue;
        s_ledges[s_ledgeN].a = a; s_ledges[s_ledgeN].b = b;
        s_ledges[s_ledgeN].up = !(F.lmBits && F.mClimbUp) || (bits & F.mClimbUp) != 0; ++s_ledgeN;
    }
    if (now < s_fakeUntil && s_ledgeN < 64) s_ledges[s_ledgeN++] = s_fake;
}
struct IRot { int pitch, yaw, roll; };
static void RefreshLadders(ULONGLONG now) {
    if (!F.ladOther || !F.rot) return;
    if (now - s_ladMs > 4000) { s_ladN = OLUE3_ObjectsOfClass("OLLadderMarker", s_lad, 256); s_ladMs = now; }
    if (now - s_ladderMs < 500) return;
    s_ladderMs = now; s_ladderN = 0;
    for (int i = 0; i < s_ladN && s_ladderN < 16; ++i) {
        void* m = s_lad[i]; void* o = nullptr; V3 a, b; IRot r;
        if (!Rd(m, F.loc, &a) || !Finite3(a) || fabsf(a.x - s_headW.x) > 800.0f || fabsf(a.y - s_headW.y) > 800.0f) continue;
        if (!Rd(m, F.ladOther, &o) || !o || !Rd(o, F.loc, &b) || !Finite3(b) || a.z >= b.z) continue;   // from its bottom marker
        if (!Rd(m, F.rot, &r)) continue;
        const float yaw = (float)r.yaw * (6.2831853f / 65536.0f);
        Ladder& L = s_ladders[s_ladderN++];
        L.bot = a; L.top = b; L.n = { cosf(yaw), sinf(yaw), 0.0f };
    }
}
// The ladder Miles stands at - at its foot on its climbing side, or on the floor behind its top - and
// the horizontal direction from him to it. -1: none.
static int LadderAt(V3 hero, V3* toward, bool* atTop, float* dist) {
    for (int i = 0; i < s_ladderN; ++i) {
        const Ladder& L = s_ladders[i];
        const float dx = L.bot.x - hero.x, dy = L.bot.y - hero.y, d = sqrtf(dx * dx + dy * dy);
        if (d > 130.0f || d < 1.0f) continue;
        const float side = -(dx * L.n.x + dy * L.n.y);           // + : on the climbing side
        if (fabsf(hero.z - L.bot.z) < 60.0f && side > 0.0f) { *toward = { dx / d, dy / d, 0.0f }; *atTop = false; *dist = d; return i; }
        if (fabsf(hero.z - L.top.z) < 60.0f && side < 0.0f) { *toward = { dx / d, dy / d, 0.0f }; *atTop = true; *dist = d; return i; }
    }
    return -1;
}
static bool HandOnLadder(const Ladder& L, V3 hw, bool atTop) {
    const float dx = hw.x - L.bot.x, dy = hw.y - L.bot.y;
    if (sqrtf(dx * dx + dy * dy) > 45.0f) return false;
    return atTop ? (hw.z > L.top.z - 70.0f && hw.z < L.top.z + 25.0f) : (hw.z > L.bot.z + 15.0f && hw.z < L.top.z + 40.0f);
}
static void ToStick(V3 w, float* sx, float* sy) { *sy = w.x * s_cy + w.y * s_sy; *sx = -w.x * s_sy + w.y * s_cy; }
static bool IsPaceSM(int sm) {   // the climbs that lift Miles: paced by the pull
    switch (sm) { case 8: case 9: case 10: case 14: case 17: case 19: case 43: case 46: return true; }
    return false;
}
// The pull speed (m/s) that plays a move at the game's own speed: one ~45 cm pull for the whole move.
// Measured: ladder mount (43) 0.92 s, ladder exit over the top (46) 2.29 s; the ledge climbs ~1.5 s.
static float PaceNominal(int sm) { return sm == 43 ? 0.49f : (sm == 46 ? 0.20f : 0.30f); }

// The closest point of an edge to p; returns the horizontal distance.
static float EdgeClosest(const Ledge& e, V3 p, V3* c) {
    const V3 ab = Sub(e.b, e.a);
    const float l2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    float t = l2 > 1e-3f ? ((p.x - e.a.x) * ab.x + (p.y - e.a.y) * ab.y + (p.z - e.a.z) * ab.z) / l2 : 0.0f;
    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    *c = Add(e.a, Mul(ab, t));
    const float dx = p.x - c->x, dy = p.y - c->y;
    return sqrtf(dx * dx + dy * dy);
}
// 1: the hand is on an edge (where it takes hold: just in front of and under the lip, so the fingers
// close over it); 2: reaching up at an edge above its reach (it holds where it is). n: from the edge
// toward you.
static int HandOnEdge(V3 hw, V3* at, V3* n, V3* edgePt, int* idx = nullptr) {
    int kind = 0; float bestScore = 1e9f;
    for (int i = 0; i < s_ledgeN; ++i) {
        V3 c; const float dh = EdgeClosest(s_ledges[i], hw, &c), dz = hw.z - c.z;
        const float hx = s_headW.x - c.x, hy = s_headW.y - c.y, headD = sqrtf(hx * hx + hy * hy);
        if (headD > 120.0f) continue;                                   // you are not at this edge
        int k = 0;
        // on the edge - and reaching for it: a hand hanging at your side next to a waist-high edge
        // (squeezing to sprint) is not a climber
        const float reach = ((hw.x - s_headW.x) * -hx + (hw.y - s_headW.y) * -hy) / (headD > 1.0f ? headD : 1.0f);
        if (dh < 22.0f && dz > -28.0f && dz < 16.0f && reach > 15.0f) k = 1;
        else if (dh < 30.0f && dz <= -28.0f && dz > -80.0f && hw.z > s_headW.z - 10.0f) k = 2;
        if (!k) continue;
        const float score = dh + fabsf(dz) + (k == 2 ? 40.0f : 0.0f);
        if (score >= bestScore) continue;
        bestScore = score; kind = k;
        if (idx) *idx = i;
        const float nl = headD > 1.0f ? headD : 1.0f;
        *n = { hx / nl, hy / nl, 0.0f };
        *edgePt = c;
        *at = k == 1 ? V3{ c.x + n->x * 5.0f, c.y + n->y * 5.0f, c.z - 3.0f } : hw;
    }
    return kind;
}
// ---- [PHYSCLIMB] (Test43) your hands carry Miles ---------------------------------------------------
// "Reverse engineer the climbing to make it more like a native VR game." Outlast climbs by playing
// root-motion animations (its special moves, Test40/41); the guided climb only paced them. Here the
// game's climb never runs. Squeeze a grip on a marked edge (OLLedgeMarker) or on a ladder's rungs
// (OLLadderMarker) and that hand is fixed to the world: Miles's body moves so it stays there, 1:1 with
// your real hand - pull down and he rises, pull in and he comes closer, move sideways and he shimmies -
// through the engine's own collision-swept move (AActor::Move), his physics off while he hangs. Two
// hands share the load, hand over hand. Get your head over a climbable edge and he hauls himself over
// onto the top (a 0.5 s mantle); a ladder keeps him at its rungs and puts him on the floor at its top.
// Let go of everything and he drops, with the push your hands gave him. Hang-only edges never let him
// over. A scripted move, a cinematic or a menu hands him back to the game at once.
// [Motion] ClimbMode=1: the Test41 guided climb; ClimbWithHands=0: the A button.
static const int kPhysNone = 0, kPhysFalling = 2;
static bool  s_pc = false;                       // the hands carry Miles
static bool  s_pcSetPhys = false;                // his physics was switched off by us
static int   s_pcEdge[2] = { -1, -1 };           // the edge a hand holds (-1: a rung of s_pcLadder)
static int   s_pcLadder = -1;
static V3    s_pcT0[2], s_pcB0[2];               // the tracked hand (m) and Miles (uu) when it took hold
static float s_pcYaw0[2] = { 0.0f, 0.0f };
static V3    s_pcPrevB = { 0, 0, 0 }, s_pcVel = { 0, 0, 0 };
static int   s_pcMantle = 0;                     // 1: up, 2: over
static bool  s_pcOnLadder = false, s_pcHangLog = false;   // (Test45) every hold is a rung; Miles holds on with no hand
static ULONGLONG s_pcMantleMs = 0, s_pcAboveMs = 0;
static V3    s_pcMantleTo = { 0, 0, 0 };
static float s_pcHalfH = 44.0f, s_pcRad = 22.0f;
static bool PcReady() { return F.phys != 0 && OLUE3_HeroMoveReady() != 0; }
static float BodyYaw() { return atan2f(s_sy, s_cy); }
static void PcSetVel(void* hero, V3 v) { if (F.vel) { WrF(hero, F.vel, v.x); WrF(hero, F.vel + 4, v.y); WrF(hero, F.vel + 8, v.z); } }
static void PcStop(const char* why, bool fall) {
    if (!s_pc && !s_pcSetPhys) return;
    void* hero = OLUE3_Hero();
    if (s_pcSetPhys && hero) {
        unsigned char ph = 0xFF;
        if (Rd(hero, F.phys, &ph) && ph == kPhysNone) {
            OLUE3_HeroSetPhysics(kPhysFalling);
            V3 v = fall ? s_pcVel : V3{ 0, 0, 0 };
            const float hs = sqrtf(v.x * v.x + v.y * v.y);
            if (hs > 260.0f) { v.x *= 260.0f / hs; v.y *= 260.0f / hs; }
            if (v.z > 260.0f) v.z = 260.0f; if (v.z < -300.0f) v.z = -300.0f;
            PcSetVel(hero, v);
        }
    }
    s_pc = false; s_pcSetPhys = false; s_pcMantle = 0; s_pcLadder = -1; s_pcAboveMs = 0; s_pcOnLadder = false; s_pcHangLog = false;
    for (int h = 0; h < 2; ++h) { s_hold[h] = false; s_holdLadder[h] = false; s_pcEdge[h] = -1; }
    ILog("[OLVR][PHYSCLIMB] %s - Miles is the game's again", why);
}
static void PcCylinder(void* hero) {
    void* cyl = nullptr; float hh = 0.0f, rr = 0.0f;
    if (F.cyl && Rd(hero, F.cyl, &cyl) && cyl && F.cylH && Rd(cyl, F.cylH, &hh) && hh > 10.0f && hh < 200.0f) s_pcHalfH = hh;
    if (cyl && F.cylR && Rd(cyl, F.cylR, &rr) && rr > 5.0f && rr < 100.0f) s_pcRad = rr;
}
// The ladder whose rungs this hand is on, anywhere up it (its markers sit at body height, so the rungs
// run from about a body below the bottom one to the top one); -1: none.
static int LadderUnderHand(V3 hw, V3 hero) {
    for (int i = 0; i < s_ladderN; ++i) {
        const Ladder& L = s_ladders[i];
        const float dx = hw.x - L.bot.x, dy = hw.y - L.bot.y;
        if (sqrtf(dx * dx + dy * dy) > 40.0f || hw.z < L.bot.z - 40.0f || hw.z > L.top.z + 110.0f) continue;   // its rails run on above the top
        const float bx = hero.x - L.bot.x, by = hero.y - L.bot.y;
        if (sqrtf(bx * bx + by * by) > 120.0f) continue;
        return i;
    }
    return -1;
}
// Over the top: up until his feet clear `upZ`, then across to `to` - the engine's swept move all the way.
static void PcMantle(ULONGLONG now, float dt, void* hero, V3 B) {
    const float el = (float)(now - s_pcMantleMs) * 0.001f;
    V3 d = { 0, 0, 0 };
    if (s_pcMantle == 1) {
        const float need = s_pcMantleTo.z - B.z;
        if (need <= 1.0f) { s_pcMantle = 2; ILog("[OLVR][PHYSCLIMB] mantle: up (%.0f uu), now over the edge", s_pcMantleTo.z - s_pcPrevB.z); }
        else d.z = need < 520.0f * dt ? need : 520.0f * dt;
    }
    if (s_pcMantle == 2) {
        const float hx = s_pcMantleTo.x - B.x, hy = s_pcMantleTo.y - B.y, hl = sqrtf(hx * hx + hy * hy);
        if (hl <= 2.0f) {
            OLXR_HapticPulse(0, 0.7f, 60); OLXR_HapticPulse(1, 0.7f, 60);
            Learn(LN_CLIMB);
            s_pcVel = { 0, 0, 0 };
            PcStop("mantled over onto the top", false);
            return;
        }
        const float st = 300.0f * dt, k = hl < st ? 1.0f : st / hl;
        d.x = hx * k; d.y = hy * k;
    }
    if (el > 1.4f) { PcStop("the mantle was blocked - he drops back", true); return; }
    float nb[3]; OLUE3_HeroMove(&d.x, nb);
}
// (Test48) [RUNGRIP] this squeeze of the left hand is a sprint: the stick is pushed and the hand is low
static bool RunGripLow(int h, const OLXrFrameHands& fh) {
    if (!g_runGripLow || h != 0 || !s_moving) return false;
    const float below = s_headT.y - P3(fh.hand[0]).y;
    if (below < 0.45f) return false;
    static ULONGLONG s_logMs = 0; const ULONGLONG now = GetTickCount64();
    if (now - s_logMs > 5000) { s_logMs = now; ILog("[OLVR][RUNGRIP] left squeeze while running, hand %.2f m below the head: sprint, not an edge", below); }
    return true;
}

static void PcStartMantle(ULONGLONG now, V3 B, V3 to, const char* what) {
    s_pcMantle = 1; s_pcMantleMs = now; s_pcMantleTo = to; s_pcPrevB = B;
    for (int h = 0; h < 2; ++h) { s_hold[h] = false; s_holdLadder[h] = false; s_pcEdge[h] = -1; }   // the hands come off
    OLXR_HapticPulse(0, 0.5f, 40); OLXR_HapticPulse(1, 0.5f, 40);
    ILog("[OLVR][PHYSCLIMB] %s: hauling himself over to (%.0f %.0f %.0f)", what, to.x, to.y, to.z);
}
static void PhysClimb(ULONGLONG now, float dt, const OLXrFrameHands& fh, V3 handW[2], bool handOk[2], int lm) {
    void* hero = OLUE3_Hero(); V3 B;
    if (!hero || !Rd(hero, F.loc, &B) || !Finite3(B)) { if (s_pc) PcStop("Miles is gone", false); return; }
    if (s_pc && !(lm == 0 || lm == 1)) { PcStop("the game took over (its own move)", false); return; }
    // the walk never carries him onto a ladder no hand has taken (the game mounts it by itself)
    if (!s_pc && lm == 0) {
        V3 tw; bool atTop = false; float ld = 1e9f;
        if (LadderAt(B, &tw, &atTop, &ld) >= 0 && ld < s_ladderBlockUU) { ToStick(tw, &s_blockX, &s_blockY); s_blockOn = true; }
    }
    if (s_pcMantle) { PcMantle(now, dt, hero, B); return; }
    const bool camHand = CamInRightHand() || g_camButton;   // (Test49) [CAMBUTTON] the right grip is the camcorder button
    // ---- hands take hold, and let go
    for (int h = 0; h < 2; ++h) {
        if (s_hold[h] && (!handOk[h] || !s_gripDown[h])) {
            s_hold[h] = false; s_holdLadder[h] = false; s_pcEdge[h] = -1; OLXR_HapticPulse(h, 0.15f, 15);
            continue;
        }
        if (s_hold[h] || !handOk[h] || (h == 1 && camHand) || (h == 0 && s_bat) || s_grab[h].d) continue;
        V3 at, n, ep; int ei = -1;
        const int k = HandOnEdge(handW[h], &at, &n, &ep, &ei);
        if (k == 1 && !s_gripDown[h]) { s_edgeNear = ep; s_edgeNearOn = true; }
        if (!s_gripEdge[h]) continue;
        const int li = k == 1 ? -1 : LadderUnderHand(handW[h], B);
        if (k != 1 && li < 0) continue;
        if (k == 1 && RunGripLow(h, fh)) continue;   // (Test48) [RUNGRIP] sprint, not the edge (a rung is still taken)
        // (Beta 1.1) [AUTOLEDGE] A tester: "Miles automatically grabbing onto a platform when jumping toward it
        // and reaching for its edge", as an option. In the air the hands do not take the edge themselves - that
        // stopped the jump dead (physics off) before the game could catch it. The game's own catch hangs him
        // from it; from there the guided hang pulls up (both grips, pull down) or A climbs, B drops.
        if (g_on[ON_AUTOLEDGE] && !s_pc && lm == 1 && k == 1) {
            static ULONGLONG s_autoLog = 0;
            if (now - s_autoLog > 2000) { s_autoLog = now; ILog("[OLVR][AUTOLEDGE] %s hand at an edge mid-air - leaving the catch to the game", h ? "right" : "left"); }
            continue;
        }
        s_hold[h] = true; s_gripEdge[h] = false;
        s_holdW[h] = k == 1 ? at : handW[h]; s_holdN[h] = n;
        s_pcEdge[h] = k == 1 ? ei : -1; s_holdLadder[h] = k != 1;
        if (k != 1) s_pcLadder = li;
        s_pcT0[h] = P3(fh.hand[h]); s_pcB0[h] = B; s_pcYaw0[h] = BodyYaw();
        OLXR_HapticPulse(h, 0.5f, 35);
        if (!s_pc) {
            s_pc = true; s_pcPrevB = B; s_pcVel = { 0, 0, 0 }; s_pcAboveMs = 0; PcCylinder(hero);
            s_pcSetPhys = OLUE3_HeroSetPhysics(kPhysNone) != 0; PcSetVel(hero, { 0, 0, 0 });
            ILog("[OLVR][PHYSCLIMB] the %s hand holds %s - your hands carry Miles (body %.0f uu wide, %.0f tall)", h ? "right" : "left",
                 k == 1 ? (ei >= 0 && ei < s_ledgeN && !s_ledges[ei].up ? "a hang-only edge" : "an edge") : "a ladder", 2.0f * s_pcRad, 2.0f * s_pcHalfH);
        }
    }
    if (!s_pc) return;
    if (!s_hold[0] && !s_hold[1]) {
        // (Test45) a ladder holds you: let go of it part-way up and Miles stays on it - take a rung to climb on;
        // at its foot, or with the crouch button, he steps off. (The user's session: let go mid-ladder twice and fell.)
        if (s_pcOnLadder && s_pcLadder >= 0 && s_pcLadder < s_ladderN) {
            OLXrPad pp = {};
            const bool dropDown = OLXR_GetPad(&pp) &&
                (pp.buttons & (OLProxy_MouseUseEnabled() ? OLXR_BTN_X : OLXR_BTN_B)) != 0 &&
                (!OLProxy_MouseUseEnabled() || pp.gripL <= 0.6f);
            if (B.z > s_ladders[s_pcLadder].bot.z + 15.0f && !dropDown) {
                if (!s_pcHangLog) { s_pcHangLog = true; ILog("[OLVR][PHYSCLIMB] both hands off the ladder part-way up - Miles holds on (take a rung to climb on, %s to drop)", OLProxy_MouseUseEnabled() ? "X" : "B"); }
                unsigned char hp = 0xFF;
                if (Rd(hero, F.phys, &hp) && hp != kPhysNone) OLUE3_HeroSetPhysics(kPhysNone);
                PcSetVel(hero, { 0, 0, 0 }); s_pcVel = { 0, 0, 0 };
                return;
            }
            s_pcVel = { 0, 0, 0 };
        }
        PcStop("both hands let go - he drops", true); return;
    }
    s_pcHangLog = false;
    unsigned char ph = 0xFF;                                          // keep his physics off (the game may switch it back)
    if (Rd(hero, F.phys, &ph) && ph != kPhysNone) OLUE3_HeroSetPhysics(kPhysNone);
    PcSetVel(hero, { 0, 0, 0 });
    // ---- where Miles has to be for every holding hand to be back on its hold
    const float yaw = BodyYaw();
    V3 want = { 0, 0, 0 }; int n = 0, upHand = -1, capHand = -1; bool ladderOnly = true;
    for (int h = 0; h < 2; ++h) {
        if (!s_hold[h]) continue;
        if (fabsf(Wrap180((yaw - s_pcYaw0[h]) * 57.2957795f)) > 0.5f) { s_pcT0[h] = P3(fh.hand[h]); s_pcB0[h] = B; s_pcYaw0[h] = yaw; }   // a turn: carry on from here
        const V3 d = Sub(P3(fh.hand[h]), s_pcT0[h]);
        const float f = -d.z * 100.0f, r = d.x * 100.0f, u = d.y * 100.0f;
        want = Add(want, Sub(s_pcB0[h], V3{ s_cy * f - s_sy * r, s_sy * f + s_cy * r, u })); ++n;
        if (!s_holdLadder[h]) {
            ladderOnly = false;
            const int ei = s_pcEdge[h];
            if (ei >= 0 && ei < s_ledgeN && s_ledges[ei].up) upHand = h; else capHand = h;
        }
    }
    want = Mul(want, 1.0f / (float)n);
    s_pcOnLadder = ladderOnly;
    const float eyeOff = s_headW.z - B.z;                              // his eye above his centre
    if (ladderOnly && s_pcLadder >= 0 && s_pcLadder < s_ladderN) {
        // a ladder: straight up and down it, at its rungs; at the top he steps onto the floor
        const Ladder& L = s_ladders[s_pcLadder];
        want.x = B.x; want.y = B.y;
        if (want.z > L.top.z - 18.0f && B.z > L.top.z - 30.0f) {
            const V3 to = { L.bot.x - L.n.x * (s_pcRad + 28.0f), L.bot.y - L.n.y * (s_pcRad + 28.0f), L.top.z + 3.0f };
            PcStartMantle(now, B, to, "the top of the ladder");
            return;
        }
    } else if (upHand >= 0) {
        // an edge he can climb: head well over its lip and he goes over
        // the edge at his chest (his eyes 50 uu over it) and he hauls himself over; earlier it lifted
        // him most of the way (Test43 testing: 12 uu over, a 2.2 m auto-lift)
        const float lip = s_holdW[upHand].z + 3.0f;
        if (s_headW.z > lip + 50.0f) { if (!s_pcAboveMs) s_pcAboveMs = now; } else s_pcAboveMs = 0;
        if (s_pcAboveMs && now - s_pcAboveMs > 200) {
            const int ei = s_pcEdge[upHand];
            V3 c = s_holdW[upHand];
            if (ei >= 0 && ei < s_ledgeN) EdgeClosest(s_ledges[ei], B, &c);
            const V3 nn = s_holdN[upHand];
            // Miles's Location is at his feet (measured: eyes 163-169 uu over it; standing on the scaffold
            // he is 2 uu under its edge marker), and the markers sit on the lip itself: his feet go to it
            const V3 to = { c.x - nn.x * (s_pcRad + 16.0f), c.y - nn.y * (s_pcRad + 16.0f), lip + 3.0f };
            PcStartMantle(now, B, to, "over the edge");
            return;
        }
    } else if (capHand >= 0) {
        // a hang-only edge: his eyes stay under its lip
        const float cap = s_holdW[capHand].z + 3.0f - 4.0f - eyeOff;
        if (want.z > cap) want.z = cap;
    }
    V3 dl = Sub(want, B);
    const float len = Len(dl);
    if (len > 45.0f) dl = Mul(dl, 45.0f / len);                       // a tracking jump never throws him
    if (len > 0.05f) { float nb[3]; OLUE3_HeroMove(&dl.x, nb); }
    V3 B2 = B; Rd(hero, F.loc, &B2);
    const V3 v = Mul(Sub(B2, s_pcPrevB), 1.0f / (dt > 0.004f ? dt : 0.004f));
    s_pcVel = Add(Mul(s_pcVel, 0.7f), Mul(v, 0.3f)); s_pcPrevB = B2;
    static ULONGLONG s_logMs = 0;
    if (now - s_logMs > 1500) {
        s_logMs = now;
        ILog("[OLVR][PHYSCLIMB] carried: hands %d%d, Miles at (%.0f %.0f %.0f), wants %+.0f %+.0f %+.0f uu, eye %.0f uu over his centre",
             s_hold[0], s_hold[1], B2.x, B2.y, B2.z, want.x - B2.x, want.y - B2.y, want.z - B2.z, eyeOff);
    }
}
static void Climb(ULONGLONG now, float dt, const OLXrFrameHands& fh, V3 handW[2], bool handOk[2]) {
    int lm = 0, sm = 0;
    OLUE3_HeroMoveState(&lm, &sm);
    s_lm = lm; s_sm = sm; s_edgeNearOn = false;
    if (InterlockedExchange(&s_fakeReq, 0)) {
        const V3 f = { s_cy, s_sy, 0.0f }, r = { -s_sy, s_cy, 0.0f };
        const V3 c = { s_headW.x + f.x * 55.0f, s_headW.y + f.y * 55.0f, s_headW.z + (s_fakeUp != 0.0f ? s_fakeUp : -45.0f) };
        s_fake.a = Sub(c, Mul(r, 30.0f)); s_fake.b = Add(c, Mul(r, 30.0f)); s_fake.up = true;
        s_fakeUntil = now + 120000; s_ledgeMs = 0;
        ILog("[OLVR][CLIMB] (dev) test edge for 2 min at (%.0f %.0f %.0f), 55 cm ahead, %+.0f cm from the eyes", c.x, c.y, c.z, c.z - s_headW.z);
    }
    if (!g_on[ON_CLIMB] || g_climbMode != 2) { if (s_pc) PcStop("physical climbing is off", false); }
    if (!g_on[ON_CLIMB]) {
        if (s_cl != CL_IDLE || s_hold[0] || s_hold[1]) { s_cl = CL_IDLE; ClearHolds(); }
        s_blockOn = false;
        if (s_paceWritten != 1.0f) { OLUE3_SetHeroAnimRate(1.0f); s_paceWritten = 1.0f; }
        return;
    }
    RefreshLedges(now);
    RefreshLadders(now);
    s_blockOn = false;
    // (Test43) [PHYSCLIMB] on his feet or in the air the hands carry him; a move the game makes itself
    // (A: its own climb, a ladder it mounted) keeps the guided handling below
    if (g_climbMode == 2 && (lm == 0 || lm == 1 || s_pc) && PcReady()) {
        if (s_cl != CL_IDLE) { ILog("[OLVR][CLIMB] %s -> %s (move %d, special %d)", kClName[s_cl], kClName[CL_IDLE], lm, sm); s_cl = CL_IDLE; }
        PhysClimb(now, dt, fh, handW, handOk, lm);
        if (s_paceWritten != 1.0f) { OLUE3_SetHeroAnimRate(1.0f); s_paceWritten = 1.0f; s_paceRate = 1.0f; }
        return;
    }
    // ---- the state follows what Miles is doing --------------------------------------------------------
    const int prev = s_cl;
    if (lm == 3) { if (s_cl != CL_LADDER) { ClearHolds(); s_cl = CL_LADDER; } }
    else if (lm == 4) { if (s_cl != CL_HANG) { s_cl = CL_HANG; s_hangHeld = false; s_bothHeldPrev = false; s_bothOffMs = 0; } }
    else if (lm == 2 && IsClimbSM(sm)) { if (s_cl != CL_CLIMBING) s_cl = CL_CLIMBING; }
    else if (s_cl == CL_PULLING) {
        if (s_stepping) {
            const float ex = s_holdW[s_stepHand].x - s_headW.x, ey = s_holdW[s_stepHand].y - s_headW.y;
            if (sqrtf(ex * ex + ey * ey) <= 38.0f || now >= s_stepUntil) {
                s_stepping = false; s_stepUntil = now; s_aFrom = now + 70; s_aUntil = now + 230;
            }
        }
        if (now - s_clMs > 1300) {
            ILog("[OLVR][CLIMB] pulled, but the game found nothing to climb here (move %d, special %d)", lm, sm);
            s_cl = CL_IDLE;
        }
    }
    else if (s_cl != CL_IDLE) { ClearHolds(); s_cl = CL_IDLE; }
    if (prev != s_cl) ILog("[OLVR][CLIMB] %s -> %s (move %d, special %d)", kClName[prev], kClName[s_cl], lm, sm);
    const bool camHand = CamInRightHand() || g_camButton;   // (Test49) [CAMBUTTON] the right grip is the camcorder button
    switch (s_cl) {
    case CL_IDLE: case CL_PULLING: {
        // (Test41) [LADDERGRAB] at a ladder: a hand that squeezes on its rungs takes it (Miles steps on
        // as you pull); otherwise the stick stops short of it - no more walking onto it by accident
        if (lm == 0) {
            V3 hero = { 0, 0, 0 }, tw = { 0, 0, 0 }; bool atTop = false; float ld = 1e9f;
            const bool heroOk = Rd(OLUE3_Hero(), F.loc, &hero);
            const int li = heroOk ? LadderAt(hero, &tw, &atTop, &ld) : -1;
            if (li >= 0) {
                for (int h = 0; h < 2; ++h) {
                    if (s_holdLadder[h] && (!s_gripDown[h] || !handOk[h])) { s_hold[h] = s_holdLadder[h] = false; continue; }
                    if (!s_hold[h] && s_gripEdge[h] && handOk[h] && !(h == 1 && camHand) && HandOnLadder(s_ladders[li], handW[h], atTop)) {
                        s_hold[h] = s_holdLadder[h] = true; s_holdW[h] = handW[h];
                        s_ladderTakeUntil = now + 800;
                        OLXR_HapticPulse(h, 0.45f, 35);
                        ILog("[OLVR][LADDERGRAB] %s hand took the ladder %s - stepping on", h ? "right" : "left", atTop ? "from the top" : "at its foot");
                    }
                }
                if ((s_holdLadder[0] || s_holdLadder[1]) && now < s_ladderTakeUntil) {
                    float sx, sy; ToStick(tw, &sx, &sy);
                    s_stepX = sx * 0.8f; s_stepY = sy * 0.8f; s_stepUntil = now + 50;
                } else if (!(s_holdLadder[0] || s_holdLadder[1]) && ld < s_ladderBlockUU) {
                    ToStick(tw, &s_blockX, &s_blockY); s_blockOn = true;

                }
            } else for (int h = 0; h < 2; ++h) if (s_holdLadder[h]) s_hold[h] = s_holdLadder[h] = false;
        }
        for (int h = 0; h < 2; ++h) {
            if (!handOk[h]) { s_hold[h] = false; continue; }
            if (s_holdLadder[h]) continue;                                  // holding a ladder, not an edge
            if (s_hold[h] && (!s_gripDown[h] || Len(Sub(handW[h], s_holdW[h])) > 55.0f || lm != 0)) {
                s_hold[h] = false; OLXR_HapticPulse(h, 0.15f, 15);
                continue;
            }
            if (s_hold[h] || lm != 0 || (h == 1 && camHand) || (h == 0 && s_bat)) continue;
            V3 at, n, ep; const int k = HandOnEdge(handW[h], &at, &n, &ep);
            if (k && !s_gripDown[h]) { s_edgeNear = ep; s_edgeNearOn = true; }
            if (k && s_gripEdge[h] && !RunGripLow(h, fh)) {   // (Test48) [RUNGRIP]
                s_hold[h] = true; s_holdW[h] = at; s_holdN[h] = n;
                OLXR_HapticPulse(h, 0.45f, 35);
                ILog("[OLVR][CLIMB] %s hand %s (edge at %.0f %.0f %.0f, hand %.0f uu below its top)", h ? "right" : "left",
                     k == 1 ? "took hold of the edge" : "reached up at an edge above", ep.x, ep.y, ep.z, ep.z - handW[h].z);
            }
        }
        // both hands hold on - or the left does and the right holds the camcorder
        const bool camOk = camHand && s_hold[0];
        if (s_cl == CL_IDLE && !s_holdLadder[0] && !s_holdLadder[1] && s_hold[0] && (s_hold[1] || camOk)) {
            // pulled: both real hands below / toward you from where they took hold
            float p = 1e9f;
            for (int h = 0; h < 2; ++h) {
                if (!s_hold[h]) continue;
                const float down = s_holdW[h].z - handW[h].z;
                const float back = (handW[h].x - s_holdW[h].x) * s_holdN[h].x + (handW[h].y - s_holdW[h].y) * s_holdN[h].y;
                const float ph = down > back * 0.8f ? down : back * 0.8f;
                if (ph < p) p = ph;
            }
            if (!s_pullArmed && p < 5.0f) s_pullArmed = true;
            if (s_pullArmed && p > 11.0f) {
                // More than ~40 cm from the edge: a half-stick step toward it first (in Miles's own frame -
                // the stick moves him relative to his facing), up to 0.35 s, and A only once he has
                // STOPPED: A while moving is the game's running jump (it threw him 2 m forward in testing
                // where there was nothing to climb), A standing still is a climb - or a hop in place.
                const int hh = s_hold[0] ? 0 : 1;
                const V3 n = s_holdN[hh];
                const float ex = s_holdW[hh].x - s_headW.x, ey = s_holdW[hh].y - s_headW.y;
                s_stepHand = hh;
                if (sqrtf(ex * ex + ey * ey) > 40.0f) {
                    const float fx = -n.x, fy = -n.y;
                    s_stepY = fx * s_cy + fy * s_sy; s_stepX = -fx * s_sy + fy * s_cy;
                    const float sl = sqrtf(s_stepX * s_stepX + s_stepY * s_stepY);
                    if (sl > 0.01f) { s_stepX *= 0.6f / sl; s_stepY *= 0.6f / sl; }
                    s_stepping = true; s_stepUntil = now + 350; s_aFrom = s_aUntil = 0;
                } else { s_stepping = false; s_stepUntil = 0; s_aFrom = now; s_aUntil = now + 160; }
                s_cl = CL_PULLING; s_clMs = now; s_pullArmed = false; s_climbByHands = true;
                OLXR_HapticPulse(0, 0.6f, 50); OLXR_HapticPulse(1, 0.6f, 50);
                Learn(LN_CLIMB);
                ILog("[OLVR][CLIMB] both hands pulled (%.0f uu) - climbing (A)", p);
            }
        }
        break;
    }
    case CL_CLIMBING:
        for (int h = 0; h < 2; ++h) s_hold[h] = handOk[h] && s_gripDown[h];   // holding on: his climbing arm
        break;
    case CL_HANG: {
        for (int h = 0; h < 2; ++h) s_hold[h] = handOk[h] && s_gripDown[h];
        const bool both = s_hold[0] && s_hold[1];
        if (both) {
            if (!s_bothHeldPrev) {
                for (int h = 0; h < 2; ++h) s_holdRefY[h] = fh.hand[h].py - fh.headRaw[1];
                s_pullArmed = true;
                if (!s_hangHeld) ILog("[OLVR][CLIMB] holding on to the ledge with both hands");
            }
            s_hangHeld = true; s_bothOffMs = 0;
            float p = 1e9f;
            for (int h = 0; h < 2; ++h) { const float ph = s_holdRefY[h] - (fh.hand[h].py - fh.headRaw[1]); if (ph < p) p = ph; }
            if (s_pullArmed && p > 0.12f) {
                s_aUntil = now + 160; s_aFrom = now; s_pullArmed = false; s_climbByHands = true;
                OLXR_HapticPulse(0, 0.6f, 50); OLXR_HapticPulse(1, 0.6f, 50);
                Learn(LN_CLIMB);
                ILog("[OLVR][CLIMB] pulled up from the ledge (%.2f m) - climbing up (A)", p);
            }
        } else if (s_hangHeld && !s_hold[0] && !s_hold[1]) {
            if (!s_bothOffMs) s_bothOffMs = now;
            else if (now - s_bothOffMs > 300) {
                s_bUntil = now + 160; s_hangHeld = false; s_bothOffMs = 0;
                ILog("[OLVR][CLIMB] let go of the ledge - dropping (B)");
            }
        } else s_bothOffMs = 0;
        s_bothHeldPrev = both;
        break;
    }
    case CL_LADDER: {
        for (int h = 0; h < 2; ++h) {
            if (!handOk[h] || !s_gripDown[h]) {
                if (s_hold[h]) { s_hold[h] = false; if (s_ladderDriver == h) s_ladderDriver = s_hold[1 - h] ? 1 - h : -1; }
                continue;
            }
            if (!s_hold[h] && s_gripEdge[h]) {
                s_hold[h] = true; s_holdW[h] = handW[h]; s_ladderDriver = h;
                OLXR_HapticPulse(h, 0.35f, 25);
                Learn(LN_CLIMB);
            }
        }
        // the hand that took a rung last pulls: Miles climbs until it is back at the rung
        s_ladderStickOn = false;
        s_holdLadder[0] = s_holdLadder[1] = false;
        if (s_ladderDriver >= 0 && s_hold[s_ladderDriver]) {
            const float err = s_holdW[s_ladderDriver].z - handW[s_ladderDriver].z;   // + : the hand is below its rung
            float st = fabsf(err) < 2.0f ? 0.0f : err / 12.0f;
            if (st > 1.0f) st = 1.0f; if (st < -1.0f) st = -1.0f;
            s_ladderStick = st; s_ladderStickOn = true;
        }
        break;
    }
    }
    // ---- [CLIMBPACE] the climb's animation speed follows the pull of the holding hands ------------------
    const float relY[2] = { fh.hand[0].py - fh.headRaw[1], fh.hand[1].py - fh.headRaw[1] };
    if (s_cl == CL_IDLE && lm == 0) s_climbByHands = false;
    const bool paceable = s_climbByHands && s_cl == CL_CLIMBING && lm == 2 && IsPaceSM(sm) && (s_hold[0] || s_hold[1]) && !s_paceGaveUp;
    if (paceable) {
        float v = 0.0f; int n = 0;
        if (s_prevRelOk && dt > 0.0f) for (int h = 0; h < 2; ++h) if (s_hold[h]) { v += (s_prevRelY[h] - relY[h]) / dt; ++n; }
        if (n) v /= (float)n;                                              // m/s, + = pulling down
        float target = v / PaceNominal(sm);
        if (target < 0.0f) target = 0.0f; if (target > 1.8f) target = 1.8f;
        s_paceRate += (target - s_paceRate) * (1.0f - expf(-dt / 0.08f));
        if (s_paceRate < 0.05f) {
            if (!s_pausedSince) s_pausedSince = now;
            else if (now - s_pausedSince > 5000) {
                s_paceGaveUp = true; s_paceRate = 1.0f;
                ILog("[OLVR][CLIMBPACE] no pull for 5 s - the game finishes the climb by itself");
            }
        } else s_pausedSince = 0;
        static ULONGLONG s_logMs = 0;
        if (now - s_logMs > 700) { s_logMs = now; ILog("[OLVR][CLIMBPACE] special %d: pull %.2f m/s -> climb speed %.0f%%", sm, v, s_paceRate * 100.0f); }
    } else {
        s_paceRate = 1.0f; s_pausedSince = 0;
        if (!(s_cl == CL_CLIMBING && lm == 2)) s_paceGaveUp = false;      // a new move gets paced again
    }
    s_prevRelY[0] = relY[0]; s_prevRelY[1] = relY[1]; s_prevRelOk = true;
    if (paceable || s_paceWritten != 1.0f) { OLUE3_SetHeroAnimRate(s_paceRate); s_paceWritten = s_paceRate; }
}
// (Test41) [LADDERGRAB] the player's own walk stick never pushes Miles onto a ladder he has not taken.
// Any thread that holds the stick (the XInput hook, the dev pad); reads only.
extern "C" void OLInteract_FilterMoveStick(float* x, float* y) {
    if (!x || !y || !s_tickMs || GetTickCount64() - s_tickMs > 250) return;
    if (s_pc) { *x = 0.0f; *y = 0.0f; return; }                       // (Test43) [PHYSCLIMB] the hands carry him
    if (!s_blockOn) return;
    const float d = *x * s_blockX + *y * s_blockY;
    if (d > 0.0f) {
        *x -= s_blockX * d; *y -= s_blockY * d;
        static ULONGLONG s_logMs = 0; const ULONGLONG now = GetTickCount64();
        if (now - s_logMs > 3000) { s_logMs = now; ILog("[OLVR][LADDERGRAB] walk held short of a ladder no hand has taken"); }
    }
}
static bool CamInRightHand() { return HolsterOn() && s_hState == H_IN_HAND; }
// [DEVBRIDGE] (Test40) "doors": every door within 15 m of Miles, nearest first (any thread; read only).
extern "C" void OLInteract_DevDoors() {
    if (!Resolve()) return;
    void* all[512]; const int n = OLUE3_ObjectsOfClass("OLDoor", all, 512);
    V3 hero = { 0, 0, 0 }; Rd(OLUE3_Hero(), F.loc, &hero);
    struct E { NearDoor d; float dist; } best[10]; int k = 0;
    for (int i = 0; i < n; ++i) {
        NearDoor nd;
        if (!ReadDoor(all[i], &nd)) continue;
        const float dist = Len(Sub(nd.knob, hero));
        if (dist > 1500.0f) continue;
        int j = k < 10 ? k++ : 9;
        if (j == 9 && k == 10 && best[9].dist <= dist) continue;
        while (j > 0 && best[j - 1].dist > dist) { best[j] = best[j - 1]; --j; }
        best[j].d = nd; best[j].dist = dist;
    }
    ILog("[OLVR][DEVDOORS] Miles at (%.0f %.0f %.0f); %d doors; nearest:", hero.x, hero.y, hero.z, n);
    for (int i = 0; i < k; ++i) {
        const NearDoor& d = best[i].d;
        ILog("[OLVR][DEVDOORS]   %4.0f uu: knob (%.0f %.0f %.0f) hinge (%.0f %.0f) ratio %.2f max %.0f%s%s%s%s", best[i].dist, d.knob.x, d.knob.y, d.knob.z,
             d.hinge.x, d.hinge.y, d.ratio, d.maxAng, d.locked ? " LOCKED" : "", d.rev ? " rev" : "", d.cant ? " cantClose" : "", d.busy ? " BUSY" : "");
    }
}
// [DEVBRIDGE] (Test40) "ledges": the nearest climbable edges to Miles (any distance), for tests.
extern "C" void OLInteract_DevLedges() {
    if (!Resolve() || !F.lmNext) { OLProxyLog("[OLVR][DEVLEDGES] ledge fields not resolved"); return; }
    s_lmkN = OLUE3_ObjectsOfClass("OLLedgeMarker", s_lmk, 1024); s_lmkMs = GetTickCount64();
    V3 hero = { 0, 0, 0 }; Rd(OLUE3_Hero(), F.loc, &hero);
    struct E { V3 a, b; float d; uint32_t bits; } best[12]; int n = 0;
    for (int i = 0; i < s_lmkN; ++i) {
        V3 a, b; void* nx = nullptr; uint32_t bits = 0;
        if (!Rd(s_lmk[i], F.loc, &a) || !Rd(s_lmk[i], F.lmNext, &nx) || !nx || !Rd(nx, F.loc, &b) || !Finite3(a) || !Finite3(b)) continue;
        if (F.lmBits) Rd(s_lmk[i], F.lmBits, &bits);
        Ledge L = { a, b }; V3 c; const float dh = EdgeClosest(L, hero, &c);
        const float d = sqrtf(dh * dh + (c.z - hero.z) * (c.z - hero.z));
        E e = { a, b, d, bits };
        int j = n < 12 ? n++ : 11;
        if (j == 11 && n == 12 && best[11].d <= d) continue;
        while (j > 0 && best[j - 1].d > d) { best[j] = best[j - 1]; --j; }
        best[j] = e;
    }
    ILog("[OLVR][DEVLEDGES] Miles at (%.0f %.0f %.0f); %d markers; nearest edges (climbUp=0x%X hang=0x%X):", hero.x, hero.y, hero.z, s_lmkN, F.mClimbUp, F.mHang);
    for (int i = 0; i < n; ++i)
        ILog("[OLVR][DEVLEDGES]   %5.0f uu: (%.0f %.0f %.0f) -> (%.0f %.0f %.0f) bits 0x%X", best[i].d, best[i].a.x, best[i].a.y, best[i].a.z,
             best[i].b.x, best[i].b.y, best[i].b.z, best[i].bits);
}

// ---- what the headset draws: how-to labels and glows first, then the developer rings -----------------
// ---- [HANDNV] (Test43) night vision on the camcorder in your hand ---------------------------------
// "Activate the night vision on free hold ... for players wanting more realism." Outlast only lets night
// vision run with the camcorder at your eye. In your hand, a right-stick click now switches the
// camcorder's own night vision: its live flip screen amplifies the low light like the real thing (your
// own eyes stay in the dark), or shows the normal colour picture. It is one switch with the game's: at
// your eye the game's night vision follows it (a click is sent if they differ), and what you set at your
// eye carries back into your hand.
static int s_handNV = -1;                       // -1: not known yet (taken from the game)
static ULONGLONG s_nvUpMs = 0, s_nvPulseUntil = 0, s_nvAdoptAt = 0;
static bool s_nvSynced = false;
extern "C" int OLInteract_HandNV() { return s_handNV > 0 ? 1 : 0; }
static void HandNV(ULONGLONG now, const OLXrPad& p, void* hero) {
    (void)p;
    // (Beta 1.1.4) [FACECAM] CamHoldGesture owns the right stick's press and hold. A short click
    // is still the night-vision switch; a held one raised/lowered the camcorder and must not also
    // toggle it.
    const bool shortClick = s_rsShort;
    s_rsShort = false;
    unsigned char mode = 0;
    const bool modeOk = F.camMode && Rd(hero, F.camMode, &mode);
    if (s_handNV < 0 && modeOk) s_handNV = mode != 0 ? 1 : 0;
    if (s_camUp == 1) {
        if (!s_nvUpMs) { s_nvUpMs = now; s_nvSynced = false; }
        // once per raise: the game's night vision takes the hand's setting (one click if they differ) ...
        if (!s_nvSynced && modeOk && s_handNV >= 0 && now - s_nvUpMs > 450) {
            s_nvSynced = true;
            if ((mode != 0) != (s_handNV > 0)) {
                s_nvPulseUntil = now + 120; s_nvAdoptAt = now + 800;
                ILog("[OLVR][HANDNV] camcorder at your eye: night vision %s, as it was in your hand", s_handNV > 0 ? "on" : "off");
            } else s_nvAdoptAt = now;
        }
        // ... after that the game's own switch (your clicks at the eye, a flat battery) is the truth
        if (s_nvSynced && modeOk && now >= s_nvAdoptAt) { s_handNV = mode != 0 ? 1 : 0; OLProxy_SetNvRequested(s_handNV); }
        return;
    }
    s_nvUpMs = 0;
    if (shortClick && CamInRightHand()) {
        s_handNV = s_handNV > 0 ? 0 : 1;
        OLXR_HapticPulse(1, 0.3f, 25);
        ILog("[OLVR][HANDNV] night vision %s on the camcorder in your hand", s_handNV ? "ON" : "OFF");
    }
}

// ---- [BREATH] (Test43) cover your mouth -------------------------------------------------------------
// The surprise: whenever Miles is breathing hard - after a run, scared, hurt, hiding in a locker -
// put a hand over your mouth and he holds his breath. His breathing (the game's own breath sounds,
// ol_sound.cpp) goes quiet under your hand and you feel his heart pound in that controller; take the
// hand away and he lets it out. Nobody holds it forever: after 12 s he gasps for air by himself.
// A hand holding something (camcorder, door, edge, battery) is not a hand over the mouth.
static bool s_cover = false, s_hushed = false; static int s_coverHand = -1;
static ULONGLONG s_coverMs = 0, s_beatMs = 0, s_dubMs = 0, s_breathCool = 0;
static V3 s_mouth = { 0, 0, 0 }; static bool s_mouthOk = false; static bool s_heavyNow = false;
static void BreathRelease(ULONGLONG now, const char* why) {
    if (s_hushed) {
        OLSound_Hush(0);
        const ULONGLONG held = now - s_coverMs;
        if (held >= 2500) OLSound_Gasp();
        if (held >= 1500) Learn(LN_BREATH);
        ILog("[OLVR][BREATH] %s after %.1f s", why, (float)held * 0.001f);
    }
    s_hushed = false; s_cover = false; s_coverHand = -1;
}
static void Breath(ULONGLONG now, const OLXrPose& head, const OLXrFrameHands& fh, int lm) {
    s_mouthOk = false; s_heavyNow = false;
    if (!g_on[ON_BREATH] || !OLSound_Ready()) { if (s_hushed || s_cover) BreathRelease(now, "cover-your-mouth off"); return; }
    s_mouth = Add(P3(head), QRot(head, V3{ 0.0f, -0.075f, -0.075f }));   // 7.5 cm under and in front of the eyes
    s_mouthOk = true;
    const V3 fwd = QRot(head, V3{ 0.0f, 0.0f, -1.0f });
    s_heavyNow = OLSound_BreathHeavy() != 0 || lm == 8;                   // breathing hard, or hiding in a locker
    // which free hand is over the mouth (enter 11 cm, leave 16 cm), in front of the face
    int hand = -1; float best = 9.0f;
    for (int h = 0; h < 2; ++h) {
        if (!(fh.valid & (1u << h)) || s_grab[h].d || s_hold[h] || (h == 1 && CamInRightHand()) || (h == 0 && s_bat)) continue;
        const V3 p = P3(fh.hand[h]);
        const float d = Len(Sub(p, s_mouth)), ahead = (p.x - s_headT.x) * fwd.x + (p.y - s_headT.y) * fwd.y + (p.z - s_headT.z) * fwd.z;
        const float lim = (s_cover && s_coverHand == h) ? 0.16f : 0.11f;
        if (d < lim && ahead > 0.01f && d < best) { best = d; hand = h; }
    }
    if (s_cover && hand < 0) { BreathRelease(now, "hand off the mouth: he lets his breath out"); return; }
    if (!s_cover && hand >= 0 && s_heavyNow && now >= s_breathCool) {
        s_cover = true; s_coverHand = hand; s_coverMs = now; s_beatMs = now; s_dubMs = 0;
        OLSound_Hush(1); s_hushed = true;
        OLXR_HapticPulse(hand, 0.25f, 30);
        ILog("[OLVR][BREATH] the %s hand covers his mouth (%.2f m) - holding his breath", hand ? "right" : "left", best);
    }
    if (!s_cover) return;
    if (now - s_coverMs > 12000) { BreathRelease(now, "he could not hold it any longer - a gasp"); s_breathCool = now + 2500; return; }
    // his heart, in the covering hand: faster the longer he holds it (80 -> 120 bpm)
    const float held = (float)(now - s_coverMs) * 0.001f, bpm = 80.0f + 40.0f * (held / 12.0f);
    if (now - s_beatMs >= (ULONGLONG)(60000.0f / bpm)) { s_beatMs = now; s_dubMs = now + 150; OLXR_HapticPulse(s_coverHand, 0.40f, 35); }
    if (s_dubMs && now >= s_dubMs) { s_dubMs = 0; OLXR_HapticPulse(s_coverHand, 0.22f, 25); }
}
static const float kLabelW = 0.13f, kLabelAspect = 512.0f / 160.0f, kLabelLift = 0.075f;
static void Markers(ULONGLONG now, const OLXrFrameHands& fh) {
    OLMarker mk[16]; int n = 0;
    auto add = [&](V3 p, float size, int col, float aspect) {
        if (n >= 16) return;
        mk[n].pos[0] = p.x; mk[n].pos[1] = p.y; mk[n].pos[2] = p.z; mk[n].size = size; mk[n].color = col;
        mk[n].aspect = aspect; ++n;
    };
    auto label = [&](V3 at, int which) { add(Add(at, V3{ 0.0f, kLabelLift, 0.0f }), kLabelW, which, kLabelAspect); };
    const bool lOk = (fh.valid & 1u) != 0, rOk = (fh.valid & 2u) != 0;
    const V3 L = P3(fh.hand[0]), R = P3(fh.hand[1]);
    // ---- [HOWTO] ----
    if (g_on[ON_HOWTO]) {
        int labels = 0;
        auto teachLabel = [&](V3 at, int which) { if (labels < 3) { label(at, which); ++labels; } };
        if (HolsterOn() && s_camUp >= 0) {
            if (s_hState == H_HOLSTERED) {
                // where the camcorder is: a glow when your right hand comes near, the label until learned
                if (rOk && s_dHolster < 0.42f) add(s_holsterVis, s_inHolster ? 0.11f : 0.09f, OLMK_GLOW, 0.0f);
                // the label floats up in front of your hip, where a glance down finds it; its arrow points at the holster
                if (Teach(LN_TAKECAM) && rOk && s_dHolster < 0.60f && s_camBlend < 0.01f)
                    teachLabel(OnBody(kHolsterOff.x, kHolsterOff.y + 0.18f, kHolsterOff.z + 0.18f), OLMK_L_TAKE_CAM);
            }
            // reload guidance: the camcorder is in your hand and the battery is low, or you have started
            bool reloadShown = false;
            if (g_on[ON_BATTERY] && s_slotValid) {
                const bool started = s_bat != 0 || !s_slotLoaded || (lOk && Len(Sub(L, s_slot)) < 0.30f);
                const bool want = started || s_energy < 0.30f || now < s_emptyFlashUntil || now < s_ejectFirstUntil;
                reloadShown = want || now < s_chargedUntil;
                if (now < s_chargedUntil) { add(s_slot, 0.08f, OLMK_GLOW, 0.0f); label(s_slot, OLMK_L_CHARGED); ++labels; }
                else if (want) {
                    if (s_bat == 0 && s_slotLoaded) {
                        if (s_numBat > 0) { add(s_slot, 0.05f, OLMK_GLOW, 0.0f); if (Teach(LN_EJECT) || now < s_ejectFirstUntil) teachLabel(s_slot, OLMK_L_PULL_OLD); }
                        else { add(s_slot, 0.05f, OLMK_GLOW_RED, 0.0f); teachLabel(s_slot, OLMK_L_NO_SPARE); }
                    } else if (s_bat == 2) {
                        add(s_slot, 0.05f, OLMK_GLOW, 0.0f);
                    } else if (s_bat == 3 && lOk) {
                        if (Teach(LN_EJECT)) teachLabel(L, OLMK_L_DROP_OLD);
                    } else if (s_bat == 0 && !s_slotLoaded) {
                        // the label floats up in front of the hip, where a glance down finds it, pointing at the pouch
                        const V3 hipLabel = OnBody(-0.20f, -0.42f, 0.12f);
                        if (s_numBat > 0) { add(s_pouch, s_inPouch ? 0.12f : 0.10f, OLMK_GLOW, 0.0f); if (Teach(LN_INSERT)) teachLabel(hipLabel, OLMK_L_TAKE_NEW); }
                        else { add(s_pouch, 0.10f, OLMK_GLOW_RED, 0.0f); teachLabel(hipLabel, OLMK_L_NO_SPARE); }
                    } else if (s_bat == 1) {
                        add(s_slot, 0.06f, OLMK_GLOW, 0.0f);
                        if (Teach(LN_INSERT)) teachLabel(s_slot, OLMK_L_SLIDE_IN);
                    }
                }
            }
            // "look through it" only while nothing about the battery is being shown
            if (s_hState == H_IN_HAND && !reloadShown && s_camUp == 0 && rOk && Teach(LN_RAISE) && s_bat == 0 && s_camBlend > 0.99f)
                teachLabel(R, OLMK_L_RAISE);
        }
        // doors: a hand near a free knob; a locked one says so while it buzzes
        // [DOOR] no guide ring, square or teach label on a knob: the door is found and opened by hand.
        // (Beta 1.1) [RECENTERHINT] Players could not find recenter ("there's no recenter key that I could find").
        // For the first 25 s of play in each session, until it has been used three times: how to do it, low
        // in front. A recenter during play counts.
        {
            static ULONGLONG s_playSince = 0; static long s_gen = -1;
            if (!s_playSince && OLUE3_GameplayActive()) s_playSince = now;   // the clock starts with play itself
            const long gen = OLXR_RecenterGeneration();
            if (s_gen >= 0 && gen != s_gen) Learn(LN_RECENTER);
            s_gen = gen;
            if (s_playSince && now - s_playSince < 25000 && Teach(LN_RECENTER)) {
                OLXrPose hd = {};
                if (OLXR_GetFrameHead(&hd)) teachLabel(Add(P3(hd), QRot(hd, V3{ 0.0f, -0.20f, -0.65f })), OLMK_L_RECENTER);
            }
        }
        // (Test43) [BREATH] breathing hard: how to hold it - in front of the face, a little low
        if (s_mouthOk && s_heavyNow && !s_cover && Teach(LN_BREATH)) {
            OLXrPose hd = {};
            if (OLXR_GetFrameHead(&hd)) teachLabel(Add(P3(hd), QRot(hd, V3{ 0.0f, -0.16f, -0.45f })), OLMK_L_COVER_MOUTH);
        }
        // (Test40) [CLIMB] a free hand at a climbable edge; holding on (or hanging), how to go up
        if (g_on[ON_CLIMB]) {
            if (s_edgeNearOn && s_cl == CL_IDLE) {
                const V3 ep = WorldToTrack(s_edgeNear);
                add(ep, 0.05f, OLMK_GLOW, 0.0f);
                if (Teach(LN_CLIMB) && !(s_hold[0] || s_hold[1])) teachLabel(ep, OLMK_L_CLIMB);
            }
            const bool holdingBoth = s_hold[0] && s_hold[1] && s_cl == CL_IDLE;
            if (Teach(LN_CLIMB) && lOk && rOk && (holdingBoth || s_cl == CL_HANG)) teachLabel(Mul(Add(L, R), 0.5f), OLMK_L_PULL_UP);
        }
    }
    // ---- developer rings ----
    if (g_on[ON_MARKERS]) {
        int doorsShown = 0;
        for (int i = 0; i < s_nearN && doorsShown < 4; ++i) {
            const NearDoor& nd = s_near[i];
            if (nd.dist > 250.0f) continue;
            bool held = (s_grab[0].d == nd.d) || (s_grab[1].d == nd.d);
            int col = nd.locked ? OLMK_RED : OLMK_GREEN;
            if (!held && !nd.locked)   // a hand within grab reach of THIS knob: squeeze now
                for (int h = 0; h < 2; ++h)
                    if (s_handOk[h] && Len(Sub(s_handW[h], nd.knob)) < kGrabUU) col = OLMK_YELLOW;
            if (held) col = OLMK_CYAN;
            else if (nd.locked && s_lockedDoor == nd.d && now < s_lockedFlashUntil) col = ((now / 120) & 1) ? OLMK_RED : OLMK_WHITE;
            add(WorldToTrack(nd.knob), held ? 0.06f : 0.045f, col, 0.0f);
            if (held) add(WorldToTrack({ nd.hinge.x, nd.hinge.y, nd.knob.z }), 0.035f, OLMK_WHITE, 0.0f);
            ++doorsShown;
        }
        // raise-to-eye zone: only while the right hand is on its way to your face
        if (s_dEye < EyeLeave() + 0.10f || (s_camUp == 1 && s_camByGesture)) {
            const int col = (s_camUp == 1 && s_camByGesture) ? OLMK_GREEN : (s_dEye < EyeEnter() ? OLMK_YELLOW : OLMK_CYAN);
            add(s_eyeZone, 0.012f, col, 0.0f);   // it sits 11 cm from your eye: keep it small
        }
        {
            int col = OLMK_ORANGE;
            if (now < s_emptyFlashUntil) col = OLMK_RED;
            else if (now < s_reloadFlashUntil || s_bat == 1) col = OLMK_GREEN;
            else if (s_inPouch) col = OLMK_YELLOW;
            add(s_pouch, 0.09f, col, 0.0f);
            if (HolsterOn()) {
                const int hc = s_hState == H_IN_HAND ? OLMK_GREEN : (s_inHolster ? OLMK_YELLOW : OLMK_CYAN);
                add(s_holster, 0.06f, hc, 0.0f); add(s_holsterVis, 0.04f, hc, 0.0f);
            }
            if (s_slotValid) add(s_slot, 0.03f, s_slotLoaded ? OLMK_WHITE : OLMK_ORANGE, 0.0f);
            if (s_bat && lOk) add(L, 0.04f, s_bat == 1 ? OLMK_YELLOW : OLMK_RED, 0.0f);
        }
    }
    OLXR_SetMarkers(mk, n);
}

// ---- [ROOMWALK] (Test44) walk around your room and Miles walks with you ---------------------------------
// ol_xr's [BODYFOLLOW] drags the position baseline along once your head passes the body bounds (and slides
// it under a head that settled somewhere new). Before Test44 that drag was dropped: the view stopped at the
// bound and the body stayed where it was. Now each drag comes here with the very pose it was taken from,
// and Miles is moved by it through the engine's own swept move (AActor::Move, as [PHYSCLIMB] uses) at the
// start of the player tick - his body and the view move in the same frame. A wall or a shut door stops him
// as it stops the stick (the view then waits at the bound, as before); a step up to kRwStepUU is taken the
// way the game's walk takes it (up, across, down), and a step down his own walk drops him onto next tick.
// Small leans stay leans: only what goes past the bounds walks.
static const float kRwStepUU = 30.0f, kRwMaxUU = 40.0f;
static bool  s_rwOk = false;
static const char* s_rwWhy = "";
static float s_rwWant = 0.0f, s_rwGot = 0.0f;
static int   s_rwSteps = 0;
static ULONGLONG s_rwLogMs = 0, s_rwStateMs = 0;
static V3    s_rwAt = { 0, 0, 0 };
// nullptr: Miles may walk with you now; else why not.
static const char* RoomWalkWhyNot() {
    if (!g_on[ON_ROOMWALK]) return "switched off";
    if (!F.ok) return "the game's fields are not read yet";
    if (!OLUE3_GetHeadPos()) return "head position is off";
    if (OLUE3_SceneScripted()) return "a scripted scene or one of the game's own moves has him";
    int lm = -1, sm = 0;
    if (!OLUE3_HeroMoveState(&lm, &sm) || lm != 0) return "he is not on his feet (ladder, ledge, door, locker, bed)";
    if (s_pc || s_cl != CL_IDLE || s_hold[0] || s_hold[1]) return "your hands are climbing";
    void* hero = OLUE3_Hero(); unsigned char ph = 0xFF;
    if (!hero || !F.phys || !Rd(hero, F.phys, &ph)) return "no body";
    if (ph != 1) return "he is off the floor (jumping, falling)";
    if (!OLUE3_HeroMoveReady()) return "AActor::Move did not verify";
    return nullptr;
}
static float HLen(V3 a, V3 b) { const float x = a.x - b.x, y = a.y - b.y; return sqrtf(x * x + y * y); }
// Game thread: from the player tick right after the frame latch, and from the interaction tick (no latch).
extern "C" void OLInteract_RoomWalkStep() {
    float d[2];
    if (!OLXR_RoomWalkTake(d)) return;
    if (!s_rwOk || !OLUE3_GameplayActive() || RoomWalkWhyNot()) return;   // dropped, as before Test44
    // the recenter frame (x right, z back, metres) into the world by the body yaw the view uses
    const float f = -d[1] * 100.0f, r = d[0] * 100.0f;
    V3 w = { s_cy * f - s_sy * r, s_sy * f + s_cy * r, 0.0f };
    float L = sqrtf(w.x * w.x + w.y * w.y);
    if (!(L > 0.01f)) return;
    if (L > kRwMaxUU) { w = Mul(w, kRwMaxUU / L); L = kRwMaxUU; }        // a hitch's worth at most
    void* hero = OLUE3_Hero(); V3 B0;
    if (!hero || !Rd(hero, F.loc, &B0) || !Finite3(B0)) return;
    float nb[3];
    if (!OLUE3_HeroMove(&w.x, nb)) return;
    V3 B = { nb[0], nb[1], nb[2] };
    float got = HLen(B, B0);
    if (got < L * 0.6f && L > 0.25f) {
        // held back: a stair, a threshold? up, across, back down - the way the game's walk steps
        const V3 up = { 0.0f, 0.0f, kRwStepUU }, rest = Mul(w, 1.0f - got / L);
        float a[3] = { B.x, B.y, B.z }, b[3] = { 0, 0, 0 }, c[3] = { 0, 0, 0 };
        if (OLUE3_HeroMove(&up.x, a) && OLUE3_HeroMove(&rest.x, b)) {
            const V3 down = { 0.0f, 0.0f, -((a[2] - B.z) + 4.0f) };
            if (OLUE3_HeroMove(&down.x, c)) {
                const V3 S = { c[0], c[1], c[2] };
                const float g2 = HLen(S, B0);
                if (g2 > got + 0.1f && S.z > B.z + 1.0f) ++s_rwSteps;
                if (g2 > got) got = g2;
                B = S;
            }
        }
    }
    s_rwWant += L; s_rwGot += got; s_rwAt = B;
}
static void RoomWalkTick(ULONGLONG now) {
    const char* why = RoomWalkWhyNot();
    const bool ok = !why;
    if ((ok != s_rwOk || (why && strcmp(why, s_rwWhy) != 0)) && now - s_rwStateMs > 1500) {
        s_rwStateMs = now;
        if (ok) ILog("[OLVR][ROOMWALK] on: walking around your room walks Miles (past %s)", s_rwWhy[0] ? s_rwWhy : "start");
        else ILog("[OLVR][ROOMWALK] paused: %s - your steps only lean the view", why);
        s_rwWhy = why ? why : "";
    }
    s_rwOk = ok;
    OLXR_RoomWalkAllow(ok ? 1 : 0);
    OLInteract_RoomWalkStep();          // without the frame latch the step happens here
    if (s_rwWant > 0.5f && now - s_rwLogMs > 3000) {
        s_rwLogMs = now;
        ILog("[OLVR][ROOMWALK] you walked %.0f cm in your room - Miles walked %.0f cm with you (%.0f cm held back by walls, %d step%s up), now at (%.0f %.0f %.0f)",
             s_rwWant, s_rwGot, s_rwWant - s_rwGot > 0.0f ? s_rwWant - s_rwGot : 0.0f, s_rwSteps, s_rwSteps == 1 ? "" : "s", s_rwAt.x, s_rwAt.y, s_rwAt.z);
        s_rwWant = s_rwGot = 0.0f; s_rwSteps = 0;
    }
}

// ---- the tick ---------------------------------------------------------------------------------------
extern "C" void OLInteract_Tick() {
    const ULONGLONG now = GetTickCount64();
    if (!OLUE3_GameplayActive() || !Resolve()) {
        // (Test43) a hand-carried climb rides out a moment of this (a hitch, a blink): the pad merge lets
        // go of him only after 1.5 s - zeroing the tick here dropped Miles off a ladder twice in testing
        if (s_pc) { static ULONGLONG s_offLog = 0; if (now - s_offLog > 2000) { s_offLog = now; ILog("[OLVR][PHYSCLIMB] gameplay check off for a moment while climbing - holding on"); } }
        else s_tickMs = 0;
        s_camStarted = false;   // re-evaluate the "not at face at start" rule next time gameplay begins
        CamcorderHudTick(false);
        s_rwOk = false; OLXR_RoomWalkAllow(0);
        OLXR_SetMarkers(nullptr, 0); return;
    }
    OLUE3_ObjScanStep();                  // (Test45) [OBJSCAN] doors, ledges, ladders: a slice a frame
    void* hero = OLUE3_Hero(); void* pc = OLUE3_PlayerController();
    unsigned char cs = 0; float z = 0.0f, en = 1.0f; int nb = 0;
    s_camUp = Rd(hero, F.camState, &cs) ? (cs == 1 ? 1 : 0) : -1;
    // (V1) the game can begin with the camcorder already at his face (CamcorderState == 1), which
    // snaps the right hand to a camcorder position on the very first gameplay frame. Never begin
    // gameplay with it at his face: force it down on the first tick so the hand is free.
    if (!s_camStarted) {
        s_camStarted = true;
        if (s_camUp == 1) {
            s_camUp = 0;
            ILog("[OLVR][FACECAM] gameplay began with the camcorder at his face - forcing it down");
        }
    }
    // (Beta 1.1.4) [FACECAM] The game's own camcorder state is the truth: a raise lands the pin,
    // a lower (ours, the game's, a reload, a cutscene) releases it.
    if (s_facePending && s_camUp == 1) {
        s_facePending = false; s_faceLock = true; s_faceLowering = false;
        OLXR_HapticPulse(1, 0.5f, 40);
        ILog("[OLVR][FACECAM] the camcorder is at his face - it stays there with your hand resting");
    } else if (s_facePending && now - s_facePendingAt > 3000) {
        s_facePending = false;
        ILog("[OLVR][FACECAM] the game did not raise the camcorder - the face pin was dropped");
    }
    if (s_faceLowering && s_camUp == 1 && now - s_faceLowerAt > 5000) {
        s_faceLowering = false;
        ILog("[OLVR][FACECAM] the game did not lower the camcorder - the face pin stays; try again");
    }
    if (s_camUp == 0 && s_faceLock) {
        s_faceLock = false; s_faceLowering = false; s_facePending = false;
        // The camcorder came off Miles's face via the game's lower animation. Leave it in the
        // normal holster path (hip) unless the player physically grabs it with the right grip.
        ILog("[OLVR][FACECAM] the camcorder is down - face pin released");
    }
    const bool zoomRead = Rd(hero, F.zoom, &z), energyRead = Rd(hero, F.energy, &en);
    const bool sparesRead = Rd(pc, F.numBat, &nb);
    s_zoom = zoomRead ? z : 0.0f;
    s_energy = energyRead ? en : 1.0f;
    s_numBat = sparesRead ? nb : 0;
    CamcorderHudTick(s_camUp >= 0 && zoomRead && energyRead && sparesRead &&
        std::isfinite(z) && std::isfinite(en));
    // [CAMHUD] "the W-T bar does not move" is answered by the log either way: one line every
    // quarter second while the zoom is actually changing shows whether the game's own factor
    // moves and where the thumb lands.
    if (s_camUp == 1 && zoomRead) {
        static float s_zoomLogged = -1.0f; static ULONGLONG s_zoomLogMs = 0;
        if (std::fabs(z - s_zoomLogged) > 0.02f && now - s_zoomLogMs > 250) {
            s_zoomLogged = z; s_zoomLogMs = now;
            ILog("[OLVR][CAMHUD] zoom %.6g -> W-T thumb %.0f%% of the lever", z, OLCamHud::ZoomUnit(z) * 100.0f);
        }
    }
    // [VIEWFINDER] engage past a small zoom, release just above zero (hysteresis)
    {
        const bool on = g_on[ON_VIEWFINDER] && s_camUp == 1 && (s_vfZoom > 0.0f ? s_zoom > 0.01f : s_zoom > 0.03f);
        s_vfZoom = on ? (s_zoom < 1.0f ? s_zoom : 1.0f) : 0.0f;
        s_vfMs = now;
    }
    float hw[3] = {}, yaw = 0.0f;
    OLXrFrameHands fh = {}; OLXrPose head = {}; OLXrPad p = {};
    if (!OLUE3_FrameView(hw, &yaw) || !OLXR_GetFrameHands(&fh) || !OLXR_GetFrameHead(&head) || !OLXR_GetPad(&p)) {
        s_tickMs = 0; s_rwOk = false; OLXR_RoomWalkAllow(0); OLXR_SetMarkers(nullptr, 0); return;
    }
    s_headW = { hw[0], hw[1], hw[2] }; s_cy = cosf(yaw); s_sy = sinf(yaw);
    s_headT = { fh.headRaw[0], fh.headRaw[1], fh.headRaw[2] };
    TorsoUpdate(head);
    // grips with hysteresis, and their press edges
    const float g[2] = { p.gripL, p.gripR };
    for (int h = 0; h < 2; ++h) {
        const bool down = s_gripDown[h] ? g[h] > 0.35f : g[h] > 0.60f;
        s_gripEdge[h] = down && !s_gripDown[h];
        s_gripDown[h] = down;
    }
    V3 handW[2]; bool handOk[2];
    for (int h = 0; h < 2; ++h) {
        handOk[h] = (fh.valid & (1u << h)) != 0;
        handW[h] = handOk[h] ? TrackToWorld(P3(fh.hand[h])) : V3{ 0, 0, 0 };
    }
    s_handW[0] = handW[0]; s_handW[1] = handW[1]; s_handOk[0] = handOk[0]; s_handOk[1] = handOk[1];
    static ULONGLONG s_lastTick = 0;
    float dt = s_lastTick ? (float)(now - s_lastTick) * 0.001f : 0.014f; s_lastTick = now;
    if (dt < 0.002f) dt = 0.002f; if (dt > 0.05f) dt = 0.05f;
    CamHoldGesture(now, p);      // (Beta 1.1.4) [FACECAM] a held right stick click: raise / pin / lower
    Holster(now, fh);           // first: a squeeze at the hip is the camcorder's, not a door's
    CamRaise(now, head, fh);
    Battery(now, fh, p);
    Climb(now, dt, fh, handW, handOk);   // (Test40) a squeeze on a climbable edge holds the edge
    RoomWalkTick(now);                    // (Test44) after Climb: it knows whether the hands are climbing
    Doors(now, dt, handW, handOk);
    Breath(now, head, fh, s_lm);          // (Test43) a hand over the mouth while he breathes hard
    HandNV(now, p, hero);                 // (Test43) night vision on the camcorder in your hand
    if (OLSound_Ready()) OLSound_Report(); // (Test43) [AUDIODIAG] every 10 s - (Test46) only with the sound hooks in
    {   // [ANCHOR] (Test40) where each hand holds on this frame: an edge (climbing), a handle, a panel
        for (int h = 0; h < 2; ++h) {
            bool on = false;
            if (s_hold[h] && (s_cl == CL_IDLE || s_cl == CL_PULLING)) { s_anchorW[h] = s_holdW[h]; on = true; }
            else if (s_doorAnchorOn[h]) { s_anchorW[h] = s_doorAnchor[h]; on = true; }
            const float rate = on ? 40.0f : 9.0f;   // stronger magnet: hand snaps to the knob faster
            s_anchorWt[h] += ((on ? 1.0f : 0.0f) - s_anchorWt[h]) * (1.0f - expf(-rate * dt));
            if (!on && s_anchorWt[h] < 0.01f) s_anchorWt[h] = 0.0f;
        }
        static ULONGLONG s_chkMs = 0; float cb[3];
        if (now - s_chkMs > 30000 && OLUE3_CameraBoneWorld(cb)) {
            s_chkMs = now;
            ILog("[OLVR][ANCHOR] view centre vs Miles's camera bone in the world: %+.1f %+.1f %+.1f uu", s_headW.x - cb[0], s_headW.y - cb[1], s_headW.z - cb[2]);
        }
    }
    {   // [DOORGRIP] the hand closes round the handle in ~0.12 s and opens in ~0.18 s
        for (int h = 0; h < 2; ++h) {
            const float target = (s_grab[h].d || (s_hold[h] && (s_cl == CL_IDLE || s_cl == CL_PULLING))) ? 1.0f : 0.0f;
            const float rate = target > s_gripW[h] ? 8.0f : 5.5f;
            s_gripW[h] += (target > s_gripW[h] ? 1.0f : -1.0f) * rate * dt;
            if (s_gripW[h] < 0.0f) s_gripW[h] = 0.0f; if (s_gripW[h] > 1.0f) s_gripW[h] = 1.0f;
        }
    }
    // A grip that is on a knob, holding a battery or reaching into the pouch is not sprint/camcorder.
    // With the holster the right grip is only ever the hand holding the camcorder.
    s_blockGrip[0] = s_grab[0].d || s_nearKnob[0] || s_bat || (g_on[ON_BATTERY] && s_inPouch && !s_moving) ||
                     (s_slotValid && Len(Sub(P3(fh.hand[0]), s_slot)) < 0.12f);
    s_blockGrip[1] = HolsterOn() || s_grab[1].d || s_nearKnob[1] || (!g_camButton && s_nearDoorWide[1]);
    // (Test40) [CLIMB] a grip holding an edge, a ledge or a rung is not sprint / the camcorder
    for (int h = 0; h < 2; ++h) if (g_on[ON_CLIMB] && (s_hold[h] || s_cl == CL_HANG || s_cl == CL_LADDER || s_cl == CL_CLIMBING)) s_blockGrip[h] = true;
    Markers(now, fh);
    s_tickMs = now;
    static ULONGLONG s_knobLogMs = 0;
    if (s_nearN > 0 && now - s_knobLogMs > 3000) {
        // where to reach: the nearest knob relative to the head (recenter frame, metres)
        s_knobLogMs = now;
        const V3 k = Sub(WorldToTrack(s_near[0].knob), s_headT);
        ILog("[OLVR][INTERACT] nearest knob %.2f m away at head%+.2f %+.2f %+.2f (x right, y up, z back) ratio %.2f%s",
             s_near[0].dist * 0.01f, k.x, k.y, k.z, s_near[0].ratio, s_near[0].locked ? " LOCKED" : "");
    }
    static ULONGLONG s_logMs = 0;
    if (now - s_logMs > 10000) {
        s_logMs = now;
        const V3 hr = Sub(s_holster, s_headT), hvr = Sub(s_holsterVis, s_headT);
        int live = 0; for (const DoorSim& s : s_sim) if (s.live) ++live;
        // the right controller's angle relative to the head (how real hands hold it: for the camcorder grip)
        const OLXrPose& rq = fh.hand[1];
        const V3 rf = QRot(rq, V3{ 0.0f, 0.0f, -1.0f }), ru = QRot(rq, V3{ 0.0f, 1.0f, 0.0f });
        ILog("[OLVR][INTERACT] alive: cam=%d zoom=%.2f vf=%.2f batteries=%d energy=%.2f | holster %s blend %.2f (reach head%+.2f %+.2f %+.2f, drawn head%+.2f %+.2f %+.2f, hand %.2f m) "
             "slot %s bat=%d | doors %d/%d (moving %d) | climb %s%s (move %d/%d, edges %d) | eye %.2f m, pouch %s | right grip fwd (%+.2f %+.2f %+.2f) up (%+.2f %+.2f %+.2f) | breath %s",
             s_camUp, s_zoom, (float)s_vfZoom, s_numBat, s_energy, s_hState == H_IN_HAND ? "IN HAND" : "holstered", s_camBlend,
             hr.x, hr.y, hr.z, hvr.x, hvr.y, hvr.z, s_dHolster, s_slotLoaded ? "loaded" : "EMPTY", s_bat, s_nearN, s_allN, live,
             kClName[s_cl], s_pc ? " (carried by the hands)" : "", s_lm, s_sm, s_ledgeN, s_dEye, s_inPouch ? "hand in" : "-", rf.x, rf.y, rf.z, ru.x, ru.y, ru.z,
             s_cover ? "HELD" : (s_heavyNow ? "heavy" : "calm"));
    }
}

extern "C" void OLInteract_Pad(const OLXrPad& p, OLInteractPad* out) {
    if (!out) return;
    out->addButtons = 0; out->blockGripL = out->blockGripR = false; out->camUp = -1;
    const ULONGLONG now = GetTickCount64();
    // A grip that went to a knob, a battery or the pouch stays the mod's until the hand actually
    // opens - whatever happens to the tick in between. Unblocking a grip that is still closed made
    // the game see a fresh press and toggle the camcorder (seen in testing). The other way round
    // too: a grip the game already has (sprint on the left grip) stays the game's while the hand
    // passes a knob or the pouch, so running through a doorway does not drop the sprint. Only a
    // grab or a battery - both start on a fresh squeeze - takes a held grip over.
    static bool s_sticky[2] = { false, false }, s_gameHas[2] = { false, false };
    const float g[2] = { p.gripL, p.gripR };
    const bool fresh = s_tickMs && now - s_tickMs <= 250;
    for (int h = 0; h < 2; ++h) {
        if (g[h] < 0.35f) { s_sticky[h] = false; s_gameHas[h] = false; }
        const bool owns = s_grab[h].d || (h == 0 && s_bat) || (h == 1 && s_hState == H_IN_HAND) || (g_on[ON_CLIMB] && s_hold[h]);
        if (fresh && (owns || (s_blockGrip[h] && !s_gameHas[h]))) s_sticky[h] = true;
        if (!s_sticky[h] && g[h] > 0.60f) s_gameHas[h] = true;
    }
    out->blockGripL = s_sticky[0];
    out->blockGripR = s_sticky[1];
    if (!fresh && s_pc && s_tickMs && now - s_tickMs <= 1500 && !OLUE3_InGameMenu()) return;   // (Test43) a hitch mid-climb: hold on
    if (!fresh) {
        // The tick stopped (menu, cutscene, loading): nothing of ours may act or stay on screen.
        static ULONGLONG s_cleared = 0;
        if (s_cleared != s_tickMs) { OLXR_SetMarkers(nullptr, 0); s_cleared = s_tickMs; }
        for (int h = 0; h < 2; ++h) if (s_grab[h].d) {
            ILog("[OLVR][INTERACT] door let go by the %s hand (interactions paused)", h ? "right" : "left");
            s_grab[h].d = nullptr;
        }
        if (s_bat == 2) s_bat = 0;
        if (s_bat == 1 && HolsterOn()) s_bat = 0;
        if (s_pc) PcStop("interactions paused (menu, cutscene, loading)", false);   // (Test43)
        if (s_hushed || s_cover) BreathRelease(now, "interactions paused");
        if (s_cl != CL_IDLE || s_hold[0] || s_hold[1]) { s_cl = CL_IDLE; ClearHolds(); }
        if (s_paceWritten != 1.0f) { OLUE3_SetHeroAnimRate(1.0f); s_paceWritten = 1.0f; s_paceRate = 1.0f; }
        s_blockOn = false;
        for (DoorSim& s : s_sim) s.live = false;
        s_anchorWt[0] = s_anchorWt[1] = 0.0f;
        return;
    }
    out->camUp = s_camUp;
    if (now < s_camPulseUntil) out->addButtons |= 0x0200;   // RB = OLA_ToggleCamcorder
    if (now < s_nvPulseUntil)  out->addButtons |= 0x0080;   // (Test43) R3 = OLA_ToggleNightVision ([HANDNV] sync)
    if (now < s_reloadUntil)   out->addButtons |= 0x8000;   // Y  = OLA_Reload
    if (now < s_aUntil && now >= s_aFrom) out->addButtons |= 0x1000;   // (Test40) A = OLA_Jump: climb (up)
    if (now < s_bUntil)        out->addButtons |= 0x2000;   // (Test40) B = OLA_CrouchToggle: drop from a ledge
    out->stickOn = false; out->stickLX = 0.0f; out->stickLY = 0.0f;
    if (g_on[ON_CLIMB] && s_cl == CL_LADDER && s_ladderStickOn) { out->stickOn = true; out->stickLY = s_ladderStick; }
    else if (now < s_stepUntil) { out->stickOn = true; out->stickLX = s_stepX; out->stickLY = s_stepY; }
}

extern "C" float OLInteract_ViewfinderZoom() {
    if (!s_vfMs || GetTickCount64() - s_vfMs > 250) return 0.0f;
    return s_vfZoom;
}
// While the camcorder is up and the right hand is at the eye - or it is pinned at his face by a
// held right stick click - the right arm is the game's.
// (V1) while Miles's own arm plays the camcorder at his face - pinned, or while the game is
// raising/lowering it - the controller hand must not override his animation.
extern "C" int OLInteract_RightArmNative() {
    if (!s_tickMs || GetTickCount64() - s_tickMs > 250) return 0;
    if (HolsterOn() && s_hState != H_IN_HAND && !s_faceLock && !s_facePending && !s_faceLowering)
        return 0;
    if (s_faceLock || s_facePending || s_faceLowering) return 1;
    return (s_camUp == 1 && s_dEye < EyeLeave() + 0.05f) ? 1 : 0;
}
extern "C" int OLInteract_FaceCamPinned() {
    if (!s_tickMs || GetTickCount64() - s_tickMs > 250) return 0;
    return (s_camUp == 1 && s_faceLock) ? 1 : 0;
}
extern "C" int OLInteract_FaceCamBusy() {
    if (!s_tickMs || GetTickCount64() - s_tickMs > 250) return 0;
    return (s_faceLock || s_facePending || s_faceLowering) ? 1 : 0;
}
// [ANCHOR] (Test40) the world point a hand holds (an edge, a handle, the panel it pushes) and how much
// (eased 0..1); ol_ue3's arm solve reaches there instead of the controller. Game thread.
extern "C" float OLInteract_HandAnchorWorld(int h, float out[3]) {
    if (h < 0 || h > 1 || !s_tickMs || GetTickCount64() - s_tickMs > 250 || s_anchorWt[h] <= 0.0f) return 0.0f;
    out[0] = s_anchorW[h].x; out[1] = s_anchorW[h].y; out[2] = s_anchorW[h].z;
    const float w = s_anchorWt[h];
    return w * w * (3.0f - 2.0f * w);
}
// [DOOR] while a hand holds a knob its wrist is pinned to the door: it turns with the swing instead of
// following the controller, so the hand never looks twisted against the handle. 1: locked this frame.
extern "C" int OLInteract_HandAnchorLock(int h) {
    if (h < 0 || h > 1 || !s_tickMs || GetTickCount64() - s_tickMs > 250) return 0;
    return (s_grab[h].d && s_grabFixed[h] && s_anchorLock[h]) ? 1 : 0;
}
// [DOOR] how far the door has swung since the grab (deg); the wrist takes this on top of the pose it had.
extern "C" float OLInteract_HandAnchorYaw(int h) {
    if (h < 0 || h > 1) return 0.0f;
    return s_anchorYaw[h];
}
// [CLIMB] (Test40) In a climb, ledge hang or on a ladder: this hand holds on (it plays the game's arm).
extern "C" int OLInteract_ClimbHold(int h) {
    if (h < 0 || h > 1 || !s_tickMs || GetTickCount64() - s_tickMs > 250 || !g_on[ON_CLIMB]) return 0;
    return (s_cl == CL_CLIMBING || s_cl == CL_HANG || s_cl == CL_LADDER) && s_hold[h] ? 1 : 0;
}
static float s_gripOverride[2] = { -1.0f, -1.0f };   // [DEVBRIDGE] "dgrip"
extern "C" void OLInteract_DevGripOverride(int h, float w) { if (h >= 0 && h < 2) s_gripOverride[h] = w; }
extern "C" void OLInteract_HandGrip(float w[2]) {
    if (s_gripOverride[0] >= 0.0f || s_gripOverride[1] >= 0.0f) {
        w[0] = s_gripOverride[0] > 0.0f ? s_gripOverride[0] : 0.0f; w[1] = s_gripOverride[1] > 0.0f ? s_gripOverride[1] : 0.0f;
        return;
    }
    const bool fresh = s_tickMs && GetTickCount64() - s_tickMs <= 1000;
    w[0] = fresh ? s_gripW[0] * s_gripW[0] * (3.0f - 2.0f * s_gripW[0]) : 0.0f;
    w[1] = fresh ? s_gripW[1] * s_gripW[1] * (3.0f - 2.0f * s_gripW[1]) : 0.0f;
}
// [HOLSTER] What ol_ue3 does with the game's handycam model this frame.
extern "C" int OLInteract_CamVisual(OLCamVisual* v) {
    if (!v) return 0;
    memset(v, 0, sizeof(*v));
    // a hitch (or a snapshot) of up to a second keeps it where it is; menus and cutscenes stop the tick
    if (!HolsterOn() || !s_tickMs || GetTickCount64() - s_tickMs > 1000 || s_camUp < 0) return 0;
    // While the game is raising / lowering the camcorder with its own arm animation, show the model
    // at the eye zone so Miles's animation is visible WITH the camera in it (not hidden, not stuck
    // on the chest while the controller hand is hidden).
    if (s_facePending || s_faceLowering) {
        v->mode = 1;
        v->blend = 1.0f;                              // fully at the eye zone
        const V3 eyeRel = { 0.035f, -0.035f, -0.11f }; // same as CamRaise's eye zone
        v->holsterRel[0] = eyeRel.x; v->holsterRel[1] = eyeRel.y; v->holsterRel[2] = eyeRel.z;
        v->torsoFwd[0] = s_torsoF.x; v->torsoFwd[1] = s_torsoF.z;
        ILog("[OLVR][FACECAM] camcorder shown at the eye during %s animation",
             s_facePending ? "raise" : "lower");
        return 1;
    }
    if (s_camUp == 1 && s_faceLock) {
        // Pinned at the face: model stays HIDDEN at the eye zone. Grab zone moves to the face so
        // the player can grab it physically exactly as from the hip.
        v->mode = 2;                              // model INVISIBLE at the eye
        const V3 eyeRel = { 0.035f, -0.035f, -0.11f }; // same as CamRaise's eye zone
        v->holsterRel[0] = eyeRel.x; v->holsterRel[1] = eyeRel.y; v->holsterRel[2] = eyeRel.z;
        v->torsoFwd[0] = s_torsoF.x; v->torsoFwd[1] = s_torsoF.z;
        return 1;
    }
    if (s_camUp == 1) { v->mode = 2; return 1; }     // at your eye: you are looking through it, model hidden
    v->mode = 1;
    const float b = s_camBlend;
    v->blend = b * b * (3.0f - 2.0f * b);            // eased glide between chest and hand
    const V3 rel = Sub(s_holster, s_headT);
    v->holsterRel[0] = rel.x; v->holsterRel[1] = rel.y; v->holsterRel[2] = rel.z;
    v->torsoFwd[0] = s_torsoF.x; v->torsoFwd[1] = s_torsoF.z;
    return 1;
}
