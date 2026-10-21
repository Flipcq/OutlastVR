#pragma once
// ol_interact.h - [INTERACT] (Test37/38) physical doors, the camcorder's chest holster, raise-to-eye,
// the VR battery reload, the camcorder viewfinder, how-to labels and the developer markers.
#include "ol_xr.h"

void OLInteract_Init();                      // reads [Motion] from outlastvr.ini; call once at startup
void OLInteract_Save(const wchar_t* ini);

// Game thread, once per frame at the end of the CPU split (after the frame's head pose is committed).
extern "C" void OLInteract_Tick();

// Game thread, from the XInput hook: buttons to add this poll, grips the game must not see (a grip
// that is grabbing a door knob, a battery or the camcorder is not sprint / camcorder), and the
// camcorder's real state (-1 = not known yet).
// (Test40) stickOn/stickLX/stickLY: the left stick the hands ask for - on a ladder, forward to climb
// to where your hand pulled; a pull on an edge, a step into it (used only while your own stick is centred).
struct OLInteractPad { unsigned short addButtons; bool blockGripL, blockGripR; int camUp; bool stickOn; float stickLX, stickLY; };
extern "C" void OLInteract_Pad(const OLXrPad& p, OLInteractPad* out);

// 0 = no viewfinder, else the camcorder zoom factor (0..1] while the zoomed picture should be shown
// in the head-locked viewfinder. Any thread.
extern "C" float OLInteract_ViewfinderZoom();
// The right arm should play the game's own camcorder animation instead of following the controller.
extern "C" int OLInteract_RightArmNative();

// [HOLSTER] (Test38) What to do with the game's handycam model this frame. Game thread.
//   mode 0: leave it to the game (holster off, menus, cutscenes)
//   mode 1: show it, `blend` of the way from the chest holster (0) to the right hand (1)
//   mode 2: hide it (it is at your eye - you are looking through it)
// holsterRel: the holster relative to the tracked head (recenter frame, metres, +x right, +y up,
// +z back); torsoFwd: the estimated body facing (x, z) in the same frame.
struct OLCamVisual { int mode; float blend; float holsterRel[3]; float torsoFwd[2]; };
extern "C" int OLInteract_CamVisual(OLCamVisual* v);
// [DOORGRIP] (Test39) how closed each hand is round a door handle (0 open .. 1 the grip), eased.
extern "C" void OLInteract_HandGrip(float w[2]);

// [ANCHOR] (Test40) the world point (uu) a hand holds - a climbable edge, a door handle, the door panel
// it pushes - and the weight (0..1, eased) with which Miles's hand reaches there instead.
extern "C" float OLInteract_HandAnchorWorld(int h, float out[3]);
// [CLIMB] (Test40) In a climb / ledge hang / on a ladder this hand holds on: it plays the game's own arm.
extern "C" int OLInteract_ClimbHold(int h);
// [LADDERGRAB] (Test41) Remove the part of a walk stick (pawn frame: x right, y forward) that would push
// Miles onto a ladder no hand has taken.
extern "C" void OLInteract_FilterMoveStick(float* x, float* y);

// 0 doors, 1 raise gesture, 2 battery, 3 viewfinder, 4 markers, 5 Quest prompts, 6 hip holster,
// 7 how-to labels, 8 (Test40) climbing with your hands, 9 (Test43) the camcorder's live screen,
// 10 (Test43) cover your mouth, 11 (Test44) walk around for real (roomscale: Miles walks with you),
// 12 (Test46) the camcorder zoom in a framed viewfinder (0: the zoomed picture fills the view)
extern "C" int  OLInteract_Get(int which);
extern "C" void OLInteract_Set(int which, int on);
// (Test43) climbing: 0 the A button, 1 guided (the game's climb at your pace), 2 physical (your hands
// carry Miles). The camcorder in your hand: 4 free hold, 3 the game's raise pose.
extern "C" int  OLInteract_ClimbMode();
extern "C" void OLInteract_SetClimbMode(int m);
extern "C" int  OLInteract_CamHold();
extern "C" void OLInteract_SetCamHold(int m);
extern "C" void OLInteract_ResetHints();     // the how-to labels show again
// (Test45) [CAMADJUST] the camcorder in your hand: rot = tilt, turn, roll (deg); off = forward, up, right (cm)
extern "C" void OLInteract_GetCamAdjust(float rot[3], float off[3]);
extern "C" void OLInteract_SetCamAdjust(const float rot[3], const float off[3]);
// [ROOMWALK] (Test44) Game thread, from the player tick after the frame latch: Miles takes the room steps
// of this frame's pose.
extern "C" void OLInteract_RoomWalkStep();
