"""Check the three climbing modes (Beta 1.2.0) and the rules PHYSICAL climbing follows.

Runs the real decision functions from src/ol_interact.cpp against synthetic values (g++), and checks
the launcher row, the shipped ini and that every call site still routes them where the design says.
Does not replace a live headset test: it proves the rules, not the game.
Run: python3 tests/climb_modes_test.py
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/ol_interact.cpp').read_text()
header = (ROOT / 'src/ol_interact.h').read_text()
proxy = (ROOT / 'src/d3d9_proxy.cpp').read_text()
ue3 = (ROOT / 'src/ol_ue3.cpp').read_text()
menu = (ROOT / 'src/ol_menu.cpp').read_text()
prompts = (ROOT / 'src/ol_prompts.cpp').read_text()
ini = (ROOT / 'runtime/outlastvr.ini').read_text()
launcher = (ROOT / 'launcher/Outlast1VR_Pages.cs').read_text()
readme = (ROOT / 'release/README.txt').read_text()

def between(text, start, end):
    return text[text.index(start):text.index(end)]

# The pure block: the mode mapping and the four rules, without any game state around them.
logic = between(source, 'enum { CLIMB_A_BUTTON', '// (Test43) [Motion] ClimbMode: 2 = physical')

stubs = r'''
#include <cassert>
#include <cmath>
#include <cstdio>
'''

checks = r'''
int main() {
    // The three modes, and a saved value from any build: 0 A button, 1 hybrid, 2 physical, anything
    // else reads as one of the three (a corrupt ini never leaves climbing unset).
    assert(ClimbModeFromSaved(0) == CLIMB_A_BUTTON);
    assert(ClimbModeFromSaved(1) == CLIMB_HYBRID);
    assert(ClimbModeFromSaved(2) == CLIMB_PHYSICAL);
    assert(ClimbModeFromSaved(7) == CLIMB_PHYSICAL);
    assert(ClimbModeFromSaved(-3) == CLIMB_A_BUTTON);
    // Only the A button turns the hands off.
    assert(!ClimbHandsOn(CLIMB_A_BUTTON));
    assert(ClimbHandsOn(CLIMB_HYBRID));
    assert(ClimbHandsOn(CLIMB_PHYSICAL));

    // A must never finish a climb in PHYSICAL - but it still jumps (nothing held, on his feet), and a
    // jump off a ladder is not a climb.
    assert(!ClimbEatsJump(CLIMB_PHYSICAL, false, false, false, 0));       // walking: A jumps
    assert(!ClimbEatsJump(CLIMB_PHYSICAL, false, false, false, 1));       // in the air: A jumps
    assert(ClimbEatsJump(CLIMB_PHYSICAL, true, false, false, 0));         // carried on an edge
    assert(!ClimbEatsJump(CLIMB_PHYSICAL, true, true, false, 3));         // carried on a ladder
    assert(ClimbEatsJump(CLIMB_PHYSICAL, false, false, true, 4));         // Miles's own grab while hanging
    assert(ClimbEatsJump(CLIMB_PHYSICAL, false, false, false, 4));        // hanging, no hold
    assert(!ClimbEatsJump(CLIMB_HYBRID, true, false, true, 4));           // hybrid keeps the button
    assert(!ClimbEatsJump(CLIMB_A_BUTTON, true, false, true, 4));

    // PHYSICAL and the walk stick: it walks him around, it walks him onto a ladder a hand is holding (so the
    // game mounts it - no "climbing it from far away"), on a ladder it only backs him down (forward is the
    // rungs'), at the top it finishes the step off, and on an edge the hands do it all.
    assert(ClimbStickRule(CLIMB_PHYSICAL, 0, false, false, 999.0f) == CLIMB_STICK_FREE);   // walking
    assert(ClimbStickRule(CLIMB_PHYSICAL, 0, true, false, 999.0f) == CLIMB_STICK_FREE);    // hand on a rung: walk to it
    assert(ClimbStickRule(CLIMB_PHYSICAL, 0, false, true, 999.0f) == CLIMB_STICK_LOCKED);  // carried on an edge
    assert(ClimbStickRule(CLIMB_PHYSICAL, 1, false, true, 999.0f) == CLIMB_STICK_LOCKED);  // the jump catch
    assert(ClimbStickRule(CLIMB_PHYSICAL, 4, false, false, 999.0f) == CLIMB_STICK_LOCKED); // hanging
    assert(ClimbStickRule(CLIMB_PHYSICAL, 5, false, false, 999.0f) == CLIMB_STICK_LOCKED); // shimmying
    assert(ClimbStickRule(CLIMB_PHYSICAL, 3, true, false, 500.0f) == CLIMB_STICK_DOWN_ONLY);  // mid-ladder
    assert(ClimbStickRule(CLIMB_PHYSICAL, 3, false, false, 41.0f) == CLIMB_STICK_DOWN_ONLY);  // still short of the top
    assert(ClimbStickRule(CLIMB_PHYSICAL, 3, false, false, 40.0f) == CLIMB_STICK_FREE);      // at the top: step off
    assert(ClimbStickRule(CLIMB_PHYSICAL, 3, false, false, -5.0f) == CLIMB_STICK_FREE);      // over it
    assert(ClimbStickRule(CLIMB_HYBRID, 3, true, true, 500.0f) == CLIMB_STICK_FREE);
    assert(ClimbStickRule(CLIMB_A_BUTTON, 4, false, false, 999.0f) == CLIMB_STICK_FREE);

    // A platform he must climb: its lip over his head. Eyes 165 uu over his feet, the shipped 10 uu
    // margin: a 175 uu lip is a climb, a 160 uu one (chest/eye height) is a jump or a vault.
    assert(EdgeNeedsClimb(165.0f + 10.0f, 165.0f, 10.0f));
    assert(!EdgeNeedsClimb(165.0f + 9.0f, 165.0f, 10.0f));
    assert(!EdgeNeedsClimb(120.0f, 165.0f, 10.0f));
    assert(!EdgeNeedsClimb(120.0f, 165.0f, 0.0f));
    assert(EdgeNeedsClimb(220.0f, 165.0f, 0.0f));

    // Near the top the game finishes the climb: his eyes 20 uu under the lip, or over it.
    assert(!ClimbHandsOff(400.0f - 21.0f, 400.0f, 20.0f));
    assert(ClimbHandsOff(400.0f - 20.0f, 400.0f, 20.0f));
    assert(ClimbHandsOff(430.0f, 400.0f, 20.0f));
    assert(!ClimbHandsOff(399.0f, 400.0f, 0.0f));     // 0: the hands finish every climb
    assert(!ClimbHandsOff(900.0f, 400.0f, 0.0f));

    // Which holding hands play the game's own arm: a guided hold always does; in PHYSICAL only Miles's
    // automatic left-hand grab does - after the grip takes that hand over it follows the controller, and
    // a released hand is never left in the game's pose.
    assert(ClimbHoldPlaysGameArm(1, false));
    assert(ClimbHoldPlaysGameArm(0, true));
    assert(!ClimbHoldPlaysGameArm(0, false));
    assert(ClimbHoldPlaysGameArm(1, true));
    // A hand stroke: one pull down is one climb, one push up is one step back - and neither repeats until
    // the hand is back near the height where it took the grip (so the game lifting Miles cannot drive it).
    bool pull = true, push = true;
    assert(HandStroke(0.0f, &pull, &push) == 0);          // no move yet
    assert(HandStroke(0.05f, &pull, &push) == 0);         // a small drift is not a pull
    assert(HandStroke(0.13f, &pull, &push) == 1);         // pulled down: one climb
    assert(!pull && push);
    assert(HandStroke(0.15f, &pull, &push) == 0);         // still down: no second climb
    assert(HandStroke(0.0f, &pull, &push) == 0);          // back at the grip: armed again
    assert(pull && push);
    assert(HandStroke(-0.13f, &pull, &push) == -1);       // pushed up: one step back
    assert(pull && !push);
    assert(HandStroke(-0.15f, &pull, &push) == 0);
    std::printf("Climb modes: 3 ways, A button / stick / edge height / hand-off / hand-stroke rules passed.\n");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'climb.cpp'
    exe = Path(tmp) / 'climb-test'
    src.write_text(stubs + logic + checks)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


# The default: the A button, in the code, the ini and the launcher.
assert 'static volatile long g_climbMode = CLIMB_A_BUTTON;' in source, 'the mod must default to the A button'
assert 'const int cm = GetPrivateProfileIntW(L"Motion", L"ClimbMode", CLIMB_A_BUTTON, ini);' in source, \
    'a missing ClimbMode must read as the A button'
assert 'ClimbWithHands=0' in ini and 'ClimbMode=0' in ini, 'the shipped ini must default to the A button'
row = launcher[launcher.index('"ClimbMode"'):]
row = row[:row.index('),')]
assert '"A BUTTON", "HYBRID", "PHYSICAL"' in row, 'the launcher must offer the three ways to climb'
assert 'new double[] { 0, 1, 2 }' in row, 'A BUTTON / HYBRID / PHYSICAL must be saved as 0 / 1 / 2'
assert launcher.count('"ClimbMode", "Climbing", 0,') == 1, 'the launcher default must be A BUTTON (0)'
assert 'A BUTTON (the default' in readme, 'the player README must name the default'

# The three ways are wired where they have to be: the in-game menu, the hints, the pad and the arms.
assert 'if (ImGui::Combo("Climbing", &ci, kClimb, 3)) OLInteract_SetClimbMode(ci);' in menu
assert '"A button (the game\'s climb)", "Hybrid (hands and the button)", "Physical (your hands only)"' in menu
assert 'case G_CLIMB_PHY: return OLInteract_ClimbMode() == 2;' in prompts, 'PHYSICAL needs its own hints (no A)'
assert 'btn &= ~ip.removeButtons;' in proxy, 'the pad must drop the A that would finish a climb'
assert 'if (OLInteract_ClimbFinishing()) return 2;' in ue3, 'the game must pose both arms while it finishes the climb'
assert 'if (c == 1 && !OLInteract_Get(7)) c = 2;' in ue3, 'the climb arms follow ClimbMode (index 7 = ClimbWithHands)'
assert 'unsigned short addButtons, removeButtons;' in header
assert 'extern "C" int OLInteract_ClimbFinishing();' in header

# PHYSICAL's own rules are the ones the design asks for, at the places they act.
assert 'if (edgeHold && !EdgeNeedsClimb(ep.z, s_headW.z, (float)g_physEdgeAboveEyes))' in source, \
    'a low edge must never start a physical climb'
assert 'if (g_climbMode == CLIMB_HYBRID && (lm == 0 || lm == 1 || s_pc || s_pcFinish) && PcReady())' in source, \
    'PHYSICAL no longer carries Miles: it lets the game\'s own climb play and only drives it with the hands'
assert 'PhysAutoCatch(now, lm, sm);' in source, \
    'PHYSICAL catches the ledge with its left hand itself, before the game hangs him'
assert 'static int PhysStroke(int h, const OLXrFrameHands& fh)' in source, \
    'a ledge stroke is measured per hand, against the head (the game moving Miles never fires one)'
assert 'if (s_lOwned && leftGrip) { const int e = PhysStroke(0, fh);' in source, \
    'the player-owned left hand drives the PHYSICAL climb like the right one'
assert 'if (s_pcAuto[0] && leftGrip) {' in source and 's_pcAuto[0] = false; s_lOwned = true;' in source, \
    'squeezing the auto-grabbed hand makes it the player\'s'
assert 's_hold[0] = s_lOwned ? leftGrip : true;' in source, \
    'letting the owned left hand go releases the ledge (the right hand may still hold it)'
assert 'if (s_lOwned) return;' in source, \
    'the auto-catch never takes the player\'s own left hand back'
assert '(s_hold[1] || (s_lOwned && s_hold[0]))' in source, \
    'the climb keeps its forward stick alive while either owned hand holds'
assert 's_hold[0] = true; s_pcAuto[0] = true; s_holdLadder[0] = false;' in source, \
    'the auto-catch keeps the ledge in the left hand'
assert 'hand pushed up - going back (walk stick back + B)' in source, \
    'a push up while the game climbs must reverse it (B), not be ignored'
assert 'if (ev < 0) s_ladderDownUntil = now + 600;' in source and \
    's_cl == CL_LADDER && GetTickCount64() < s_ladderDownUntil' in source, \
    'going down a ladder plays the game\'s own descent animation with both arms its own'
assert 'ClimbHoldPlaysGameArm(guided ? 1 : 0, physical)' in source, \
    'a physical hold stays in the game\'s arm (auto-grab and carry alike)'
assert 's_pcT0[h] = P3(fh.hand[h]); s_pcB0[h] = B' in source, 'the carry anchors at the controller hand (same frame/units), not the ledge point'
assert 's_pcT0[h] = s_holdW[h]' not in source and 's_pcT0[0] = s_holdW[0]' not in source, \
    'the frame-mixing anchor (world ledge point vs recenter hand) must be gone'
assert 'if (ladderOnly && s_hold[0] != s_hold[1]) return;' in source, \
    'one hand on a ladder holds still - the climb needs both (hand over hand)'
assert 's_ladderDriver = s_holdLadder[0] ? 0 : (s_holdLadder[1] ? 1 : -1);' in source, \
    'the rung held while stepping on drives the first pull once the game mounts the ladder'
assert 'PcFindLedge(lm == 4, lm == 4 ? s_headW.z : s_eyeGroundZ' in source, \
    'the auto-catch measures the climb against the height he took off from, and relaxes it while hanging'
assert 'RelaxFingers(space, mesh, h, 1.0f)' in ue3 and 'if (moveClass == 1)' in ue3, \
    'a free hand during a climb rests open, not in the game\'s gripping fingers'
assert 'const int rule = ClimbStickRule((int)g_climbMode, s_lm, ladderHold, edgeCarry, s_climbTopUnder);' in source, \
    'the stick follows where he is (walk, mount, down a ladder, the step off)'
assert 'if (rule == CLIMB_STICK_DOWN_ONLY && *y > 0.0f)' in source, 'mid-ladder only the back of the stick climbs'
assert 's_climbTopUnder = s_ladders[li].top.z - hb.z;' in source, 'the ladder top decides the step off'
assert 'ld < LadderWalkBlock()' in source, \
    'PHYSICAL stops the walk short of a ladder when the ini sets a barrier'
assert 'ClimbHandsOff(s_headW.z, lip, (float)g_physHandsOffUU)' in source, 'the game finishes the climb at the top'
assert 's_aFrom = now; s_aUntil = now + 200;' in source, 'the hand-off asks the game for its own climb (A)'
assert 'PhysClimbEdgeAboveEyes' in ini and 'PhysClimbHandOffUU' in ini, 'the two numbers must be documented'
assert 'PHYSICAL  Only your hands' in readme, 'the player README must describe the three ways'

print('Climb modes: A BUTTON default, HYBRID and PHYSICAL wired end to end; rules and call sites checked.')
