"""Run the real HYBRID hand-carry code and the PHYSICAL rules (ol_interact.cpp) against a fake hero.

Compiles the actual climb block with g++ and drives it: in HYBRID a low edge must not start a climb, a tall
one must, the body follows the hands, crouch drops him and near the top the game finishes; in PHYSICAL the
game does the climbing and the hands ask for it - the left hand catches the ledge by itself in the air (no
delay) and the player's grip takes it over, either hand climbs (pull down) or goes back (push up), letting go
with nothing else holding drops him, and on a ladder a pull is one rung, a push is one down (the game's own
descent animation) while the game lifting him is none. Fakes the engine, not the rules.
Run: python3 tests/climb_phys_test.py
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/ol_interact.cpp').read_text()

pure = source[source.index('enum { CLIMB_A_BUTTON'):source.index('static volatile long g_camHold = 4;')]
phys = source[source.index('static int LadderAt('):source.index('\nstatic void Climb(')]

stubs = r'''
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstddef>

using ULONGLONG = unsigned long long;
struct V3 { float x, y, z; };
struct OLXrPose { float qx, qy, qz, qw; float px, py, pz; };
struct OLXrFrameHands { OLXrPose hand[2]; float headRaw[3]; unsigned valid; };
struct OLXrPad { float moveX, moveY, lookX, lookY, trigL, trigR, gripL, gripR; unsigned buttons; };
enum { OLXR_BTN_B = 1u << 1, OLXR_BTN_X = 1u << 2 };

static V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 Mul(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static float Len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
static V3 P3(const OLXrPose& p) { return { p.px, p.py, p.pz }; }
static float Wrap180(float d) { while (d > 180.0f) d -= 360.0f; while (d < -180.0f) d += 360.0f; return d; }
static bool Finite3(V3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// ---- the fake engine: one actor, its location, velocity and Physics byte
struct FakeActor { V3 loc; float vel[3]; unsigned char phys; };
static FakeActor g_actor = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 2 };
static int g_heroPhysicsSet = 0, g_heroMoves = 0;
struct Offsets { size_t phys, vel, loc, cyl, cylH, cylR; };
static const Offsets F = { 1, 100, 200, 300, 400, 500 };
static void* g_heroPtr = &g_actor;
static bool Rd(void* o, size_t off, V3* out) { (void)off; if (!o) return false; *out = g_actor.loc; return true; }
static bool Rd(void* o, size_t off, unsigned char* out) { (void)off; if (!o) return false; *out = g_actor.phys; return true; }
static bool Rd(void* o, size_t off, void** out) { (void)o; (void)off; *out = nullptr; return true; }
static bool Rd(void* o, size_t off, float* out) { (void)o; (void)off; *out = 0.0f; return false; }
template <class T> static bool WrF(void* o, size_t off, T v) {
    (void)o;
    if (off >= F.vel && off < F.vel + 12) g_actor.vel[(off - F.vel) / 4] = (float)v;
    return true;
}
extern "C" void* OLUE3_Hero() { return g_heroPtr; }
extern "C" int OLUE3_HeroMoveReady() { return 1; }
extern "C" int OLUE3_HeroSetPhysics(int phys) { g_actor.phys = (unsigned char)phys; ++g_heroPhysicsSet; return 1; }
extern "C" int OLUE3_HeroMove(const float d[3], float out[3]) {
    ++g_heroMoves; g_actor.loc.x += d[0]; g_actor.loc.y += d[1]; g_actor.loc.z += d[2];
    out[0] = g_actor.loc.x; out[1] = g_actor.loc.y; out[2] = g_actor.loc.z; return 1;
}
'''

state = r'''
// ---- the module state the extracted code expects around it
static V3 s_headW = { 0, 0, 0 };
static V3 s_headT = { 0.0f, 0.0f, 0.0f };
static long g_runGripLow = 1;
static float s_cy = 1.0f, s_sy = 0.0f;
static bool s_moving = false;
struct Ledge { V3 a, b; bool up; };
static Ledge s_ledges[64]; static int s_ledgeN = 0;
struct Ladder { V3 bot, top, n; };
static Ladder s_ladders[16]; static int s_ladderN = 0;
static bool s_hold[2] = { false, false }, s_holdLadder[2] = { false, false };
static V3 s_holdW[2], s_holdN[2];
static bool s_gripDown[2] = { false, false }, s_gripEdge[2] = { false, false };
struct GrabFake { void* d; };
static GrabFake s_grab[2] = { { nullptr }, { nullptr } };
static int s_cl = 0, s_lm = 0;
static ULONGLONG s_aFrom = 0, s_aUntil = 0, s_bUntil = 0;
static bool s_climbByHands = false;
static V3 s_edgeNear; static bool s_edgeNearOn = false;
static bool s_blockOn = false; static float s_blockX = 0.0f, s_blockY = 0.0f;
static bool s_crouchPrev = false;
static ULONGLONG s_crouchCooldown = 0;
static bool s_hangHeld = false, s_bothHeldPrev = false, s_pullArmed = true;
static ULONGLONG s_clMs = 0, s_bothOffMs = 0;
static float s_ladderStick = 0.0f; static bool s_ladderStickOn = false;
static ULONGLONG s_stepUntil = 0; static float s_stepX = 0.0f, s_stepY = 0.0f;
static float s_holdRefY[2] = { 0.0f, 0.0f };
static bool IsClimbSM(int sm) { return sm == 14 || sm == 17 || sm == 43 || sm == 46; }
enum { ON_AUTOLEDGE = 12 };
static bool g_on[13] = { false };
static long g_camButton = 0;
enum { LN_CLIMB = 5 };
static void Learn(int) {}
static bool CamInRightHand() { return false; }
static bool HolsterOn() { return true; }
static void ILog(const char* fmt, ...) { (void)fmt; }
static void OLXR_HapticPulse(int, float, float) {}
static ULONGLONG g_now = 1000;
static ULONGLONG GetTickCount64() { return g_now; }
static bool g_padCrouch = false;
static bool OLXR_GetPad(OLXrPad* p) { memset(p, 0, sizeof(*p)); if (g_padCrouch) p->buttons = OLXR_BTN_B; return true; }
static bool OLProxy_MouseUseEnabled() { return false; }
static float LadderWalkBlock() { return 0.0f; }
'''

checks = r'''
// A ledge `lipZ` high, 30 uu in front of Miles, running left-right.
static void MakeEdge(float lipZ, bool up) {
    s_ledges[0].a = { -60.0f, 30.0f, lipZ };
    s_ledges[0].b = {  60.0f, 30.0f, lipZ };
    s_ledges[0].up = up;
    s_ledgeN = 1;
}
// A ladder 30 uu in front of him: its rungs climb 0..300 uu.
static void MakeLadder() {
    s_ladders[0].bot = { 0.0f, 30.0f, 0.0f };
    s_ladders[0].top = { 0.0f, 30.0f, 300.0f };
    s_ladders[0].n = { 0.0f, -1.0f, 0.0f };
    s_ladderN = 1;
}
// A tracked frame: the hands' and the head's height (m, recenter frame).
static OLXrFrameHands HandFrame(float l, float r, float head) {
    OLXrFrameHands fh = {};
    fh.hand[0].py = l; fh.hand[1].py = r; fh.headRaw[1] = head;
    return fh;
}
// One frame with both hands where the test put them (handW), the squeeze flags as set.
static float g_handY[2] = { 0.0f, 0.0f };      // the tracked hands' height (m, recenter frame)
static void Reset(int lm, int mode = CLIMB_HYBRID) {
    s_ledgeN = 0; s_ladderN = 0;
    s_hold[0] = s_hold[1] = false; s_holdLadder[0] = s_holdLadder[1] = false;
    s_gripDown[0] = s_gripDown[1] = false; s_gripEdge[0] = s_gripEdge[1] = false;
    s_edgeNearOn = false; g_padCrouch = false; s_lm = lm;
    g_actor.loc = { 0.0f, 0.0f, 0.0f }; g_actor.phys = 2;
    g_actor.vel[0] = g_actor.vel[1] = g_actor.vel[2] = 0.0f;
    g_climbMode = mode; g_physEdgeAboveEyes = 10; g_physHandsOffUU = 20;
    g_on[ON_AUTOLEDGE] = true;
    g_handY[0] = g_handY[1] = 0.0f;
    s_hangHeld = false; s_bothHeldPrev = false; s_pullArmed = true; s_bothOffMs = 0;
    s_stPull[0] = s_stPull[1] = s_stPush[0] = s_stPush[1] = true;
    s_stHeld[0] = s_stHeld[1] = false;
    s_lOwned = false; s_strokeActMs = 0; s_ladderDownUntil = 0; s_dropPulseAt = 0;
    s_pcAuto[0] = s_pcAuto[1] = false;
    s_catchBlockUntil = 0; s_eyeGroundZ = 165.0f;
    s_aFrom = s_aUntil = s_bUntil = s_stepUntil = 0;
    s_stepX = s_stepY = 0.0f;
}
// One frame with both hands where the test put them (handW), the squeeze flags as set.
static void Step(int lm, V3 h0, V3 h1) {
    OLXrFrameHands fh = {}; V3 handW[2] = { h0, h1 }; bool handOk[2] = { true, true };
    fh.hand[0].px = 0.0f; fh.hand[0].py = g_handY[0];
    fh.hand[1].px = 0.2f; fh.hand[1].py = g_handY[1];
    g_now += 16; s_lm = lm;
    PhysClimb(g_now, 0.016f, fh, handW, handOk, lm);
    s_gripEdge[0] = s_gripEdge[1] = false;
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // progress as it happens, even if a check fails

    // His feet at 0 and his eyes at 165 uu (the measured 1.65 m) unless a step says otherwise.
    s_headW = { 0.0f, 0.0f, 165.0f };

    // 1) A LOW edge is a jump or a vault: a hand reaching a chest-high lip takes nothing, because the
    //    edge has to be over his head (PhysClimbEdgeAboveEyes).
    Reset(0); MakeEdge(150.0f, true);
    s_gripDown[0] = true; s_gripEdge[0] = true;
    Step(0, { 0.0f, 30.0f, 150.0f }, { 0.0f, 30.0f, 150.0f });
    assert(!s_hold[0] && !s_pc && g_heroPhysicsSet == 0);
    Reset(0); MakeEdge(165.0f, true);                 // right at his eyes: still not a climb
    s_gripDown[0] = true; s_gripEdge[0] = true;
    Step(0, { 0.0f, 30.0f, 165.0f }, { 0.0f, 30.0f, 165.0f });
    assert(!s_hold[0] && !s_pc);
    std::printf("low edge: refused (a jump or a vault, the game's own).\n");

    // 2) A ledge over his head, reached up at (HandOnEdge kind 2): the squeeze takes it and the hands
    //    carry him, his physics off.
    Reset(0); MakeEdge(200.0f, true);
    s_gripDown[0] = true; s_gripEdge[0] = true;
    Step(0, { 0.0f, 30.0f, 165.0f }, { 0.0f, 30.0f, 165.0f });
    assert(s_hold[0] && s_pc && g_actor.phys == 0 && !s_pcAuto[0]);
    std::printf("tall edge: the hand takes it, his physics off, the hands carry him.\n");

    // 3) Pushing down with the hands (the push-up) raises him.
    {   const float z0 = g_actor.loc.z;
        g_handY[0] = -0.10f; g_handY[1] = -0.10f;      // both hands 10 cm lower, still on the ledge
        Step(0, { 0.0f, 30.0f, 175.0f }, { 0.0f, 30.0f, 175.0f });
        assert(g_actor.loc.z > z0); }
    std::printf("pull: the body rises with the hand.\n");

    // 3b) A ladder: a hand on a rung takes it while his feet are still on the floor, and the walk stick is
    //     his again so he can walk onto it (the game mounts it) instead of climbing it from far away. On the
    //     ladder the stick only backs him down, and at the top it finishes the step off.
    Reset(0); MakeLadder();
    g_handY[0] = g_handY[1] = 0.0f;
    s_gripDown[0] = true; s_gripEdge[0] = true;
    Step(0, { 0.0f, 30.0f, 60.0f }, { 0.3f, 30.0f, 60.0f });
    assert(s_hold[0] && s_holdLadder[0] && s_pc);
    assert(ClimbStickRule(CLIMB_PHYSICAL, 0, true, false, 999.0f) == CLIMB_STICK_FREE);
    assert(LadderNearHero(g_actor.loc) == 0);
    assert(ClimbStickRule(CLIMB_PHYSICAL, 3, true, false, 300.0f - 120.0f) == CLIMB_STICK_DOWN_ONLY);
    assert(ClimbStickRule(CLIMB_PHYSICAL, 3, true, false, 300.0f - 280.0f) == CLIMB_STICK_FREE);
    std::printf("ladder: a rung is taken from the floor (the stick walks him on), back down it, and the top steps off.\n");

    // 4) The crouch button still drops him.
    Reset(0); MakeEdge(200.0f, true);
    s_gripDown[0] = true; s_gripEdge[0] = true;
    Step(0, { 0.0f, 30.0f, 165.0f }, { 0.0f, 30.0f, 165.0f });
    assert(s_pc);
    g_padCrouch = true;
    Step(0, { 0.0f, 30.0f, 165.0f }, { 0.0f, 30.0f, 165.0f });
    assert(!s_pc && g_actor.phys == 2);
    std::printf("crouch: he lets go and his physics is the game's again.\n");

    // 5) PHYSICAL (no carry at all): hanging from a ledge (the game's catch, move 4), his LEFT hand takes
    //    the ledge by itself and the right one stays free. His body stays the game's.
    Reset(4, CLIMB_PHYSICAL); MakeEdge(190.0f, true);
    s_headW = { 0.0f, 0.0f, 170.0f };
    PhysAutoCatch(g_now, 4, 0);
    assert(s_hold[0] && s_pcAuto[0] && !s_hold[1] && !s_pc && g_actor.phys == 2);
    std::printf("hang: Miles's own left hand holds the ledge, the right one is free.\n");

    // 5a) While the game already hangs him the lip is AT his head (not over it): the grab still takes it at
    //     once - the hang itself proves it is a climb, so there is no wait for the over-the-head rule.
    Reset(4, CLIMB_PHYSICAL); MakeEdge(170.0f, true);                 // the lip right at his eyes
    s_headW = { 0.0f, 0.0f, 170.0f };
    PhysAutoCatch(g_now, 4, 0);
    assert(s_hold[0] && s_pcAuto[0]);
    std::printf("hang at the lip: the left hand grabs immediately (no over-the-head wait).\n");

    // 5b) The left-hand takeover: squeezing the auto-grabbed hand makes it the player's - it climbs like the
    //     right one, and letting it go drops him unless the right hand holds on.
    {
        bool ok[2] = { true, true };
        Reset(4, CLIMB_PHYSICAL); MakeEdge(190.0f, true);
        s_headW = { 0.0f, 0.0f, 170.0f };
        PhysAutoCatch(g_now, 4, 0);
        assert(s_pcAuto[0] && s_hold[0]);
        s_gripDown[0] = true;
        OLXrFrameHands fh = HandFrame(0.0f, 0.0f, 0.0f);
        PhysHang(g_now, fh, ok);
        assert(s_lOwned && !s_pcAuto[0] && s_hold[0] && !s_hold[1]);
        fh = HandFrame(-0.13f, 0.0f, 0.0f);                            // the left pull: climbing
        PhysHang(g_now, fh, ok);
        assert(g_now >= s_aFrom && g_now < s_aUntil && s_stepY > 0.9f);
        std::printf("left takeover: the auto-grabbed hand is the player's and it climbs.\n");

        // The right hand is on the ledge: letting the left go does not drop him.
        Reset(4, CLIMB_PHYSICAL); MakeEdge(190.0f, true);
        s_headW = { 0.0f, 0.0f, 170.0f };
        PhysAutoCatch(g_now, 4, 0);
        s_gripDown[0] = true;
        fh = HandFrame(0.0f, 0.0f, 0.0f); PhysHang(g_now, fh, ok);
        assert(s_lOwned && s_hold[0]);
        s_gripDown[0] = false; s_gripDown[1] = true;
        fh = HandFrame(0.0f, 0.0f, 0.0f); PhysHang(g_now, fh, ok);
        assert(!s_hold[0] && s_hold[1]);
        g_now += 400;                                                  // long past the drop's 180 ms
        fh = HandFrame(0.0f, 0.0f, 0.0f); PhysHang(g_now, fh, ok);
        assert(s_bUntil == 0 && s_hold[1]);
        std::printf("left released, right holding: he stays on the ledge.\n");

        // Nothing holds the ledge: letting the left hand go drops him (B), and nothing re-grabs him.
        Reset(4, CLIMB_PHYSICAL); MakeEdge(190.0f, true);
        s_headW = { 0.0f, 0.0f, 170.0f };
        PhysAutoCatch(g_now, 4, 0);
        s_gripDown[0] = true;
        fh = HandFrame(0.0f, 0.0f, 0.0f); PhysHang(g_now, fh, ok);
        assert(s_lOwned);
        s_gripDown[0] = false;
        fh = HandFrame(0.0f, 0.0f, 0.0f); PhysHang(g_now, fh, ok);      // no hold: the drop waits 150 ms
        g_now += 200;
        fh = HandFrame(0.0f, 0.0f, 0.0f); PhysHang(g_now, fh, ok);      // and he drops
        assert(g_now < s_bUntil && s_stepY < -0.9f);
        assert(s_dropPulseAt > g_now);                                  // the second buzz follows as he falls
        s_lOwned = false;                                              // the game ends the hang
        PhysAutoCatch(g_now, 4, 0);                                    // the drop is his own: no re-grab
        assert(!s_pcAuto[0] && !s_hold[0]);
        std::printf("left released, nothing holding: he drops (B), and no hand re-grabs.\n");
    }

    // 6) The right hand's pull down is the climb: A (the game's climb-up) and the walk stick forward (the
    //    push onto the ledge). Its push up is the way back: B and the stick back.
    {
        Reset(4, CLIMB_PHYSICAL); MakeEdge(190.0f, true);
        s_headW = { 0.0f, 0.0f, 170.0f };
        bool ok[2] = { true, true };
        s_gripDown[1] = true;
        OLXrFrameHands fh = HandFrame(0.0f, 0.0f, 0.0f);
        PhysHang(g_now, fh, ok);
        assert(s_hold[1] && !s_pc);
        fh = HandFrame(0.0f, -0.13f, 0.0f);                            // pulled 13 cm down
        PhysHang(g_now, fh, ok);
        assert(g_now >= s_aFrom && g_now < s_aUntil && s_stepY > 0.9f);
        assert(g_now < s_stepUntil);
        std::printf("hang pull: the right hand climbs (A + the stick forward).\n");

        fh = HandFrame(0.0f, 0.0f, 0.0f); PhysHang(g_now, fh, ok);     // back to the grip: armed again
        g_now += 400;                                                  // one action per gesture (300 ms)
        fh = HandFrame(0.0f, 0.13f, 0.0f);                             // pushed 13 cm up
        PhysHang(g_now, fh, ok);
        assert(g_now < s_bUntil && s_stepY < -0.9f);
        std::printf("hang push: the right hand goes back (B + the stick back).\n");

        // 6a) The same push while the game is climbing reverses the climb, and a pull from either hand asks
        //     it to keep climbing.
        Reset(4, CLIMB_PHYSICAL); MakeEdge(190.0f, true);
        s_headW = { 0.0f, 0.0f, 170.0f };
        s_gripDown[1] = true;
        fh = HandFrame(0.0f, 0.0f, 0.0f); PhysHang(g_now, fh, ok);
        fh = HandFrame(0.0f, -0.13f, 0.0f); PhysHang(g_now, fh, ok);   // the pull: climbing
        fh = HandFrame(0.0f, 0.0f, 0.0f); PhysClimbDrive(g_now, fh, ok);
        g_now += 400;
        fh = HandFrame(0.0f, 0.13f, 0.0f); PhysClimbDrive(g_now, fh, ok);
        assert(g_now < s_bUntil && s_stepY < -0.9f);
        std::printf("mid-climb push: the right hand reverses the climb (B + the stick back).\n");
    }

    // 7) A chest-high edge is not caught while jumping: only a ledge over the height he took off from (a
    //    platform he has to climb) is; a low one stays the game's vault.
    Reset(1, CLIMB_PHYSICAL); MakeEdge(140.0f, true);
    s_headW = { 0.0f, 0.0f, 165.0f };
    PhysAutoCatch(g_now, 1, 0);
    assert(!s_hold[0] && !s_pc && g_actor.phys == 2);
    std::printf("jump at a chest-high edge: no catch (the game vaults it).\n");

    // 7b) Jumping at a ledge that needs climbing, the left hand catches it in the air, at once (no wait for
    //     the game's hang) - but nothing is frozen: his body stays the game's.
    Reset(1, CLIMB_PHYSICAL); MakeEdge(190.0f, true);
    s_headW = { 0.0f, 0.0f, 170.0f };                 // his eyes rose with the jump; the lip was over them
    PhysAutoCatch(g_now, 1, 0);
    assert(s_hold[0] && s_pcAuto[0] && !s_pc && g_actor.phys == 2);
    PhysAutoCatch(g_now, 0, 0);                       // he lands: the hand lets go
    assert(!s_hold[0] && !s_pcAuto[0]);
    std::printf("jump: the left hand catches the ledge in the air, no freeze, no delay.\n");

    // 7c) On a ladder each hand stroke is one rung: the game lifting Miles (his hand and head together) is
    //     not a step, so the held hand never runs off to the next rung by itself.
    {
        Reset(3, CLIMB_PHYSICAL); MakeLadder();
        bool ok[2] = { true, true }; V3 hw[2] = { { 0.0f, 30.0f, 60.0f }, { 0.3f, 30.0f, 60.0f } };
        s_gripDown[0] = true;
        OLXrFrameHands fh = HandFrame(0.0f, 0.0f, 0.0f);
        PhysLadder(g_now, fh, hw, ok, false);
        assert(s_hold[0]);
        fh = HandFrame(0.2f, 0.0f, 0.2f);             // the game lifts him: hand and head rise together
        PhysLadder(g_now, fh, hw, ok, false);
        assert(s_stepUntil == 0);
        fh = HandFrame(-0.13f, 0.0f, 0.0f);           // a real pull down: up a rung
        PhysLadder(g_now, fh, hw, ok, false);
        assert(s_stepY > 0.9f);
        fh = HandFrame(0.0f, 0.0f, 0.0f); PhysLadder(g_now, fh, hw, ok, false);
        fh = HandFrame(0.13f, 0.0f, 0.0f);            // a push up: down a rung
        PhysLadder(g_now, fh, hw, ok, false);
        assert(s_stepY < -0.9f && g_now < s_ladderDownUntil);   // the game's own descent animation plays
        std::printf("ladder: a pull is one rung up, a push one down (the game's own descent), the game's own lift none.\n");
    }

    // 8) Near the top the game is asked to finish the climb (A sent, the hands off, physics still off).
    Reset(0); MakeEdge(200.0f, true);
    s_headW = { 0.0f, 0.0f, 165.0f };
    s_gripDown[0] = true; s_gripEdge[0] = true;
    Step(0, { 0.0f, 30.0f, 165.0f }, { 0.0f, 30.0f, 165.0f });
    assert(s_pc);
    g_actor.loc.z = 40.0f; s_headW.z = 203.0f;         // he pulled himself up: eyes at the lip
    Step(0, { 0.0f, 30.0f, 165.0f }, { 0.0f, 30.0f, 165.0f });
    assert(s_pcFinish == 1 && !s_pc && !s_hold[0] && g_now >= s_aFrom && g_now < s_aUntil);
    assert(g_actor.phys == 0);                        // held while the game picks it up
    Step(2, { 0.0f, 30.0f, 165.0f }, { 0.0f, 30.0f, 165.0f });   // its climb special move plays
    assert(s_pcFinish == 2);
    Step(0, { 0.0f, 30.0f, 165.0f }, { 0.0f, 30.0f, 165.0f });   // and it is over: walking again
    assert(s_pcFinish == 0 && g_actor.phys == 2);
    std::printf("top: the game finishes the climb, then Miles is his own again.\n");

    // 9) 0 in PhysClimbHandOffUU: the hands finish every climb themselves (no A sent to the game).
    Reset(0); MakeEdge(200.0f, true); g_physHandsOffUU = 0;
    s_headW = { 0.0f, 0.0f, 165.0f };
    s_gripDown[0] = true; s_gripEdge[0] = true;
    Step(0, { 0.0f, 30.0f, 165.0f }, { 0.0f, 30.0f, 165.0f });
    g_actor.loc.z = 40.0f; s_headW.z = 203.0f;
    Step(0, { 0.0f, 30.0f, 165.0f }, { 0.0f, 30.0f, 165.0f });
    assert(s_pcFinish == 0 && s_pc);
    std::printf("hand-off off: the hands keep the climb.\n");

    std::printf("Physical climb: HYBRID carry (edge height, pull, crouch, hand-off), PHYSICAL left-hand catch,\n"
                "left-hand takeover/release drop, either-hand climb/reverse and ladder strokes passed.\n");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'physclimb.cpp'
    exe = Path(tmp) / 'physclimb-test'
    src.write_text(stubs + state + pure + phys + checks)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
                    '-Wno-misleading-indentation', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
