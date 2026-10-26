# Outlast 1 VR native-source baseline

This project extends an earlier GPL-3.0 Outlast VR source archive.

## Source provenance

- Original archive: `outlast-vr-mod-master.zip`
- Original archive SHA-256: `076BEC496CE6CE255E9550C5284878FE0DE28B29D7C10AA970D62D16B666A4AE`
- Imported: 2026-09-19
- Upstream license: GPL-3.0
- Upstream copyright and vendored dependency notices remain intact.

## Clean baseline changes

- Preserved the upstream same-frame native stereo renderer and OpenXR camera hook.
- Added a shared body-boundary clamp to the recentered OpenXR position before both the UE3 camera and compositor consume it.
- Kept limited positional 6DoF for natural leaning and crouching.
- Disabled experimental arm/skeleton driving.
- Disabled experimental hand-directed camera takeover.
- Enabled Touch-controller virtual-gamepad input.
- Set the narrow-view cinema threshold to 45 degrees so startup/menu views use the comfortable screen while gameplay and wider scripted scenes stay native stereo VR.
- Added a muted-blue branded cinema backplate using the supplied StrikeZoneVR logo. The
  only visible credit text is `VR mod made by StrikeZoneVR`.

The failed wrapper/hybrid experiments from Tests 10-13 are not part of this source tree or package.

## Phase 2 (Test 35) changes

- Headset-resolution rendering: `FViewport::UpdateViewportRHI` is hooked so the engine sizes its
  windowed backbuffer at 2 x the runtime's recommended eye size (`[Render] HighResRender`).
- The game's D3D9 runs on Windows' D3D9On12 layer (`[XR] D3D9On12`), and finished frames reach the
  D3D11 bridge through a shared D3D12 texture plus shared fences (`[XR] ZeroCopy`). The old D3D9
  readback, now asynchronous, remains as the fallback.
- Scene-state reflection (pause, cinematic, scripted movement, the game's menu stack) puts every
  menu on the cinema screen and hands scripted scenes back to the game's own arm animation.
- Developer test bridge (`[Debug] DevBridge`, off by default) for scripted input and snapshots.
- New found-footage launcher (`launcher/`), applying the same ini profile as `Outlast-VR.bat`.

## Phase 2 (Test 36) changes

Fixes for the Test 35 headset report ("jitters and stuttering", "after the car the arms don't move").

- Arms (`[NAMESFIX]`): GNames is a TArray the engine reallocates when a level load adds names. Test 35
  found it at the main menu and cached the data pointer; after the save loaded every name lookup read
  the freed block, `Miles_beheaded` was never recognised and the IK never ran. Lookups now re-read the
  in-exe `{Data, Num, Max}` slot. `[IKDIAG]` logs why no arm solve happened (every 5 s, capped).
- Jitter (`[FRAMETAG]`): UE3 builds views on the game thread and presents on the render thread (logged
  thread ids differ). Test 35 promoted/tagged the head pose at Present, so each image carried the NEXT
  frame's pose (measured with a synthetic head turn: 236 of 236 frames wrong). The game thread now
  latches the pose once per frame (`OLXR_FrameLatch`: player tick, IK, split start) and records it at
  split end keyed by the left FSceneView's view+projection matrices; the render thread identifies the
  frame in the `[PVS]` hook (InitViews passes its FViewInfo copy) and Present uses that exact record.
  `[XR] FramePoseLatch=0` restores Test 35.
- Lens-shaped eyes (`[LENSFIT]`): FSceneView's constructor (RVA 0x8BF640, arg 18 = projection,
  16-byte aligned) gets each eye's own off-centre frustum during the split's eye passes; the bridge maps
  each half 1:1 (`BlitFitToEyeTan`) and the UI mirror centres each eye's box on its straight-ahead.
  Same pixel cost, no ~1.24x stretch. `[Render] LensFit=0` = old centred render.
- Frame rate (`[D3D9EX]` + `[EXSHARE]`): measured at the asylum gate on the null headset with
  publishing paused: native D3D9 78 fps, D3D9On12 46 (render thread bound by 9On12 translation).
  The game now gets a D3D9Ex device; every MANAGED create is translated (textures -> DEFAULT+DYNAMIC,
  buffers -> DEFAULT). Frames are StretchRect'ed into 6 shared render targets, published when their
  D3D9 event query completes, and opened by the D3D11 bridge (legacy shared handle) - no CPU/GPU sync.
  Result with frames delivered: 74-81 fps (was ~46). `[XR] D3D9Ex=0` falls back to D3D9On12+ZeroCopy.
- `[CPUSPLIT]`, `[XRTIMING]` and `[FRAMETAG]` lines every 5 s; DevBridge commands `hands`, `headwave`,
  `lensfit`, `latch`, `occl`; `[Debug] FakeEyeFov=1` gives a symmetric test headset Quest 3 frusta.

## Phase 2 (Test 37) changes

Test 36 report: "zoom in and out does nothing", "the right stick moves the view up and down like an
Xbox controller"; request: motion controls for doors, camcorder and battery reload, with developer
markers, feeling like a native VR game; the game's hints in Quest 3 words.

- Zoom (`[VIEWFINDER]`): the game zooms by narrowing the camera FOV, which the per-eye VR FOV
  override discarded. While `OLHero.CurrentCamcorderZoomFactor` (+0x31AC) > 0.03 (exit < 0.01) with
  the camcorder up, `Hook_GetFovAngle` renders 2*atan((1-z)*tan30 + z*tan4), LensFit is skipped and the
  frame carries flag bit1; the bridge shows head-locked 60-degree quads cropped to 4:3.
- Stick pitch: `[Input] StickPitch` defaults to 0; `LevelPitch` nudges the right stick to level any
  pitch the game keeps (deadzone 1.5 deg) in gameplay while the camcorder is down.
- Interactions (`ol_interact.cpp`, `[INTERACT]`, ticked at split end on the game thread):
  - Mapping: the camera hook records the head-centre world position and base yaw at eye phase 1;
    world = head + fwd*(-dz*100) + right*(dx*100) + up*(dy*100) for recenter-frame metres.
  - Doors: `OLObjectsOfClass("OLDoor")` every 2 s, doors within 4 m with DoorType (+0x308) 0. Knob =
    native 0xCB24D0 `(door, float* out)`, hinge = door mesh (+0x258) LocalToWorld origin (+0xF0).
    Grab on a grip edge within 26 uu; while held r = r0 + sign*Wrap180(a - a0)/MaxOpenAngle (+0x2BC),
    sign = -1 (+1 if bReverseDirection), written to TargetOpenRatio (+0x190C) - the door's own tick
    swings it. Locked = bLocked && !bFakeUnlocked (bitfield +0x2C4). Release on grip open or slip > 45 uu.
  - Camcorder: eye zone = head + R(head)*(0.035, -0.035, -0.11); raise after 140 ms within
    `[Motion] RaiseToEyeReachM` (0.14), lower beyond reach + 0.14 if raised by gesture; presses RB.
    `OLInteract_RightArmNative` hands the right arm to the game's pose while raised at the eye.
  - Battery: pouch = head + yaw-only (-0.20 right, -0.58 up, +0.04 back); grip edge in the pouch while
    not moving takes one if `OLPlayerController.NumBatteries` (+0xAA0) > 0; release within 0.18 m of
    the right hand presses Y; gestures pause 2.5 s for the reload, then re-arm once.
  - Grips: a grip that started a grab/battery stays blocked until it opens (unblocking a closed grip
    was a fresh RB press); a grip the game already had (sprint) stays the game's past knobs.
  - Markers: OpenXR quad layers (6 colour ring textures) in LOCAL space, alpha-blended; haptics per hand.
- Hints (`ol_prompts.cpp`, `[PROMPTS]`): UOLUtils' token resolver (RVA 0xD8BF90,
  `FString* (FString* out, const FString* in)`, reads `in` only) turns `{OLA_x}` into `{X}` (pad icon)
  or `(E)`. The hook passes a rewritten copy: whole INT hints replaced while their gesture is on, every
  other token named as its Quest 3 control (the d3d9_proxy button map). `[Motion] QuestPrompts`.
- DevBridge: `hand`, `grip`, `look`, `setf`, `tp`; DevGet prints struct values.

## Phase 2 (Test 38) changes

Request: a camcorder holster ("pick up smoothly and bring to face"), a reworked VR reload with
indicators, then the launcher with more settings and a Quest 3 controller layout in words.
Choices: right-chest holster, let go = back to the holster, eject then insert, teach-then-fade labels.

- Holster (`[HOLSTER]`, ol_ue3 `CamPlace` + ol_interact `Holster`):
  - OLHero.CameraMesh (+0x2CF0, SkeletalMesh `handycam`) hangs from Miles's body mesh at bone 52
    `Hero-R-Hand_aux`; the game keeps it HiddenGame=1 in first person whether raised or not.
    CameraMeshShadowProxy (+0x2CF8) hangs off the hidden ShadowProxy body the same way.
  - Shown/hidden with `UPrimitiveComponent::SetHiddenGame` = vtable slot 0x4C0 (execSetHidden at
    RVA 0x2516D0, prologue checked), game thread only.
  - Placed every frame inside the arm-solve hook by rewriting its FAttachment (stride 0x34: comp,
    FName, RelativeLocation, RelativeRotation, RelativeScale) in SkeletalMeshComponent.Attachments
    (+0x3EC): Rel = BoneCS^-1 * Target, FQuat->FRotator as UE3's FMatrix::Rotator.
  - Targets: in the hand = the right grip pose through the arm's own mapping (body basis *
    PoseGripCanonical); on the chest = Miles's right collarbone root + (-17 up, 11 fwd, 10 right) uu.
    Blend eased 0..1 (0.18 s out, 0.28 s back), hidden while CamcorderState == 1.
  - The camera bone is ~30 cm ahead of Miles's torso, so the drawn chest model maps back ~18 cm
    behind the real-chest reach point; ol_interact accepts a grab at either.
  - Holster mode blocks the right grip from the game; raise = model at the eye zone (reach 0.14 m).
- Reload (`[RELOAD]`): slot = right grip + R(0, 0.03, 0.07) m. Left grip at the slot + pull > 8 cm =
  eject (visual only: the game battery keeps running), release = drop; pouch only gives a new one
  with the slot empty; new battery within 7 cm of the slot = Y pulse (game reload). A NumBatteries
  drop (Y fallback) re-loads the slot.
- Body facing: a lazy torso yaw (head yaw with a +-35 deg dead zone) carries the holster and pouch.
- How-to (`[HOWTO]`): one OpenXR quad atlas (rings, glows, 10 GDI-drawn labels; 18 separate
  swapchains failed on SteamVR); labels counted per action in [Motion] Learned* (3 = learned).
- Launcher: `Outlast1VR_Pages.cs` (partial Viewfinder): TAPE 02 VR SETTINGS (24 rows, ini via
  Get/WritePrivateProfileString) and TAPE 03 QUEST 3 CONTROLS (procedural Touch Plus drawings from
  Meta's product photos, animated callouts, gesture cards). Dev flags `--page`, `--page-sel`,
  `--page-age`, `--settings-test DIR`.
- DevBridge: `oget`, `raw`, `hide`, `camtune`.

## Beta 1.0 (Test 39) changes

Test 38 report: camcorder on the hip, reload = button, camcorder not in the native hand pose, doors
should push Boneworks-style with a natural grip pose, notes close to gameplay on click, the body
drifts so recentering is frequent. This build is the public beta.

- Holster on the right hip: visual from Miles's right thigh joint (bone 67) + (0 up, -1 fwd, 21 right)
  uu, pitched 20 deg; reach point (0.20 right, -0.60 up, 0 fwd) m from the head (torso yaw).
- `[NATIVEGRIP]`: while the camcorder is up at the eye (right arm native) the game's grip is captured
  ~0.9 s in: finger locals (bones 37-51) and the hand aux bone 52 relative to the wrist 36 (bone 52 is a
  child of 36). Baked default from this build's capture. Holding it lowered: wrist target =
  controllerCam * gripAux^-1 (lens follows the controller), fingers + aux set to the captured locals,
  attachment offset 0. The camcorder placement now runs after the helper rebase (Test 38 ran before).
- `[DOORGRIP]`: the same finger locals plus extra bend about local -Z (thumb 0/18/22, fingers
  38/48/32 deg) on a hand holding a knob; the left hand mirrors the right across the rig's one mirrored
  local axis (axis 2, found from the reference pose's finger offsets: keep q.z, negate q.x/q.y, negate t.z).
- `[DOORPUSH]`: the door panel = hinge -> knob; an open hand's signed distance from it picks a side once
  clear of it (> 9 uu), then TargetOpenRatio keeps the panel 7 uu ahead of the hand on that side.
- `[MENUPRESS]`: in the game's menus (OLUE3_InGameMenu: menu/paused/no hero) presses become 70 ms taps
  and buttons held from a menu are silent after it until released. The note view closes the notebook
  when the opening A is still down (raw pad: 300 ms closes, 80 ms does not). Pause = Menu RELEASE
  without the left grip.
- `[BODYFOLLOW]`: the position baseline is dragged with the head past the body bounds (x, z) and decays
  toward the head (tau ~9 s) after 2.5 s still. `[Stage3] BodyFollow`.
- Defaults: BatteryGesture=0 (Y reloads), DevMarkers=0. DevBridge: vrbtn, trig, bones, gripdump, dgrip,
  curl, headoff. BUILD=OUTLAST1VR-BETA-1.0-20261014.

## Beta 1.0 final (Test 40) changes

Beta 1.0 (Test 39) headset report: the camcorder looks wrong in the hand (the back of Miles's hand / his
sleeve over it), climbing should use the hands (with a setting for the old way), jumping should keep the
hands with the game's own hand animation, doors are janky and need physics. Release held for these.

- `[FREEMOVE]`: OLPawn.LocomotionMode (+0x7BC, ELocomotionMode: 0 Walk 1 Fall 2 SpecialMove 3 Ladder
  4 LedgeHang 5 LedgeWalk 6 Squeeze 7 Door 8 Locker 9 Cinematic ...) and OLPawn.SpecialMove (+0x7BD,
  ESpecialMoveType, 101 moves) decide the arms: jumps, falls, landings, vaults, crouches keep the
  player's arms (Test 39 handed every non-walk mode to the game); climbs / ledge hang / ladder: a
  holding hand plays the game's arm, a free hand stays the player's; doors, lockers, beds, squeezes,
  cutscenes stay native. The finger animation is the game's throughout (the IK moves whole subtrees).
- `[CAMHOLD]` 3 (default): matched to the game's own camcorder raise animation (captured with
  [Stage3] ArmDrive=0, CamcorderHolster=0, pad RB, HMD frames): forward = 75% Miles's facing (camera
  bone forward) + 25% eye->hand, 35% of the eye->hand pitch, upright, then roll 12 deg about X and a
  -60 deg turn about Z (g_camHold3, camtune 4) - from the eye the flip screen is face on with the left
  side (dial, ports) and the fingers over the top, as in the game. The wrist follows (camcorder = wrist *
  gripAux); the controller's rotation is ignored. Mode 2 (controller aim, upright) was the user's
  "unnatural" build; Test 39 copied the controller's roll as well. Note: ArmDrive=0 also leaves the
  reflection registry unresolved (fields MISSING) - test-only configuration.
  Mode 1 (hand follows the controller, camcorder in its grip) put the lens sideways/backwards with the
  arm calibration's wrist and is kept only as a dev switch (`camhold`).
- `[DOORPHYS]`: doors a hand touches are simulated (angle, angular speed): push = one-sided contact with
  a 1600 deg/s^2 limit (Miles's hand stops at the panel while the door catches up), side locked until the
  hand leaves the panel; handle = spring (k 160, c 24); hinge friction viscous 3.2/s + 70 deg/s^2; soft
  bounce at the open stop, latch at closed, stops against Miles (26 uu). Driven through 0xCC46A0
  (OLDoor set ratio + target + navmesh obstacle, the game's own procedural-swing call). Doors the game is
  using (DoorState 3/4, break state, DoorUser, bPlayingLocked/Blocked/AutoCloseAnim, bAITraversing) or
  that the game moves itself are handed back at once.
- `[CLIMB]` ([Motion] ClimbWithHands=1): edges = OLLedgeMarker -> Next (+0x24C) chains, flags at +0x260
  (bCanClimbUp 0x8, bCanLedgeHang 0x4), OLGameplayMarker.bEnabled +0x240/0x1. A fresh squeeze with the
  hand reaching onto an edge (22 uu across, -28..+16 uu height, reaching >= 15 uu toward it) holds it
  (world anchor); both held (or left + camcorder in the right hand) and pulled 11 uu down/toward you ->
  if the edge is > 40 uu away a 0.6 stick step toward it (<= 0.35 s, until 38 uu), then A standing still
  (A while moving is the running jump: 2 m forward in testing); the game picks the move.
  Ledge hang: both grips hold, a 12 cm pull -> A, both released 0.3 s after holding -> B. Ladder: a
  squeeze holds the rung where the hand is; the left stick is driven by (rung z - hand z) / 12 uu until
  the hand is back at the rung (tested: 35 cm pulls climb 33 cm, exit on top runs).
- `[ANCHOR]`: OLInteract_HandAnchorWorld -> the arm solve blends the hand target to a world point through
  Miles's component LocalToWorld (+0xC0). Checked: view centre == camera bone world (0.0 uu).
- Labels: CLIMB, PULL YOURSELF UP; hints rewritten for climbing (G_CLIMB). Launcher: "Climb with your
  hands" setting, CLIMB gesture card (replaces the button reload card). DevBridge: enum, cls lists
  functions, hrot, camhold, ledges, doors, fakeledge.

## Test 41 changes (Beta 1.0 package)

User: "when I walk up to any climbing sections it auto goes into that climbing animation and feels weird
with motion controls; the climbing should feel kind of like Boneworks". The log showed ladder auto-mounts
(SMT 43/44) and auto exits (45/46).

- `[CLIMBPACE]`: during the lifting climb moves (SMT 8, 9, 10, 14, 17, 19, 43, 46) with a hand holding on,
  SkeletalMeshComponent.GlobalAnimRateScale (+0x2C4; Mesh +0x444 and ShadowProxy +0x2CE0) = holding hands'
  downward speed / PaceNominal(sm) (43: 0.49 m/s, 46: 0.20, ledges 0.30 - one ~45 cm pull per move),
  EMA 80 ms, 0..1.8; no hand holding -> 1; paused 5 s -> 1 for that move; restored to 1 outside climbs
  and when the interaction tick stops. Measured: the ladder mount takes 0.92 s at 100%, 2.68 s at 25%
  (root motion; a small part is time-based); the top exit 2.29 s.
- `[LADDERGRAB]`: ladders = OLLadderMarker (+0x244 OtherMarker, Actor.Rotation +0x8C yaw: points to the
  climbing side). Within 100 uu of a ladder foot (climbing side) or top (floor side), with no hand on it,
  OLInteract_FilterMoveStick removes the stick component toward it (the game mounts from ~90 uu walking
  in - a 90 uu block window never caught it). A fresh squeeze within 45 uu of its axis takes it: stick
  toward it for 0.8 s -> the game's mount, paced. Dev: ladderblock.
- Note: OLUE3_FindProp rejects a non-bool field when a mask pointer is passed - pass nullptr for floats.

## Test 42 changes (Beta 1.0 package)

User: "the body and arms don't do great following your movements - sometimes the arms deform from moving
around a lot or crouching". `[ARMFIX]` (g_armFix mask, dev "armfix", default 15):
- 8 steady twist helpers: the forearm / upper-arm twist chains (29/11, 31/10, 53/35, 55/34) were
  PoseRebaseHelper'd from the LIVE native pose each frame; measured native helper->driver drift (ARMDIAG):
  standing < 5 deg, walking / sprinting 12-19, crouching / crouch-walking up to 31 (left forearm). Now
  placed by their relation in the reference pose (captured with PoseCaptureReference). Hand aux 28/52
  drift was 0 and stays live (the camcorder grip uses 52).
- 1 hinge elbows: the upper arm turns by IQFromFrames(upper dir, hinge axis) instead of the shortest arc,
  hinge = cross(shoulder->wrist, elbow offset); the forearm then turns about the hinge only.
- 2 the wrist roll's twist about the forearm axis goes 50% into the forearm subtree, the rest at the wrist.
- 4 collarbone: turns toward last frame's target by 0.45 * (height above the shoulder / arm length) +
  0.25 * (reach past 80% of arm length), at most 25 deg.

## Test 43 changes (Beta 1.0 package)

User (Test42 video/screenshot): camcorder "does not go in front of my face / not much free movement",
"reverse engineer the climbing ... like a native VR game, keep the old option", "the first jumpscare at the
door does not happen when I open it with motion controls", "a surprise motion feature", "the storm is way
louder". Choices: free-hold camcorder with a live screen; cover your mouth.

RE facts (OLGame.exe):
- `AOLDoor::execTriggerEvent` 0xCA4980 -> vtable +0x750 = `AOLDoor::TriggerEvent(byte, Pawn)` (0xD66D90 in
  this build, hooked from a door's vtable): walks GeneratedEvents for OLSeqEvent_Door and calls
  CheckActivate with ActivateIndices = [type] (output link = type).
- Door open 0xD4A770 (Instigator rdx): TriggerEvent(0) if DoorState==0, DoorState=1, TriggerEvent(1).
  Close 0xD58AEB: TriggerEvent(2), DoorState=2. Locked try 0xCDE640: DoorState=4, TriggerEvent(3).
  DoorState: 0 shut, 1 open, 2 closing, 3 player interacting, 4 animating. AI bashing fires 5 / 6.
- ESpecialMoveType (103): 24/25 Enter/ExitSqueeze, 28 EnterDoorInteraction, 29 OpenDoorInstant,
  30 OpenDoorPartial, 31 TryOpenLockedDoor, 32 RunThroughDoor, 33 CloseDoor, 38/39 Locker, 40/41 Bed,
  49 PickupObject, 51-53 Struggle, 62-70 grabbed/thrown/killed/dying (full list in the Test43 log).
- EPhysics: 0 None 1 Walking 2 Falling 4 Flying 9 Ladder. Actor.Physics +0xA8.
- `AActor::Move(Delta)` 0x652FF0 (= GWorld 0x14200B5A8 ->MoveActor 0x509DF0, flags 0); setPhysics = vtable
  +0x420 (NewPhysics, NewFloor, &NewFloorV).
- Miles's Location is at his feet: eyes 163-169 uu over it; standing on the scaffold he is 2 uu under the
  platform's edge marker. OLLedgeMarker sits on the lip; OLLadderMarker at the standing height (bottom
  -191 / top 108.7 on the scaffold ladder). CylinderComponent: radius 30, half-height 82.5.
- Wwise: OLGame.exe EXPORTS AK::SoundEngine (PostEvent id/name/wide, ExecuteActionOnEvent,
  SetListenerPosition, SetObjectObstructionAndOcclusion, SetState, SetRTPCValue, ...). The game posts by
  wide name. Miles's breath: VO_MILES_RUN_BREATH (+ VO_MILES_RUN_STOP 0.4 s later) after a sprint;
  STRESS_BREATH / BRE_* / pain breaths from scripts. Chris Walker = "Soldier" (Play_SOL_*).

New code: `[DOOREVENT]` (ol_ue3 hook + ol_interact open/close mirroring, 2 s hands-off after X),
`[PHYSCLIMB]` (ol_interact; ClimbMode 2/1/0), `[CAMHOLD]` mode 4 + `[FLIPSCREEN]` (ol_ue3 publishes the
screen pose, ol_xr draws a 384x288 night-vision crop quad; g_flipScr 1.5 -8.2 3.2 uu, 0.062 x 0.046 m),
`[BREATH]` (ol_sound.cpp Wwise hooks + ol_interact gesture), `[AUDIODIAG]`. Dev: `climbmode`, `doorev`,
`flipscr`, `phys`, `hmove`, `fakeledge [cm]`, `camtune 5`.

Night vision (later Test43): `[HANDNV]` - right-stick click on the camcorder in hand switches the flip
screen's night vision; synced once per raise with OLHero.CamcorderMode (+0x303D: 0 CCM_Default,
1 CCM_PoweredNightVision, 2 CCM_NightVision) by an R3 pulse, then the game's mode is adopted
(OLProxy_SetNvRequested). The hero's lamps: NVLightPowered +0x2D40, NVLightDefault +0x2D48, DarkLight
+0x2D50 (SpotLightComponents). `[NVEYES]` - the lamp's lighting is cut at each eye's outer side under
[LENSFIT]'s off-centre frustums (ruled out: aim, shadows of the hands, scissor - FD3D9DynamicRHI::
SetScissorRect 0xBF4C00 hook kept, off, dev "scissorfix"); while NV is up the eyes render symmetric.

## Test 44 changes: walk around for real (roomscale)

User: "when you move in real life in VR the body in game moves with you too" (they test it on the Quest).
`[ROOMWALK]`: what `[BODYFOLLOW]` drags the position baseline by (past the body bounds / the 2.5 s settle)
is handed to Miles instead of dropped. ol_xr: `g_rwCum` (XR thread) -> `g_rwPend` with the pending pose ->
`g_rwLat` at PromotePendingPose; `OLXR_RoomWalkTake` (game thread) returns the latched total since the last
take; `OLXR_RoomWalkAllow` (ol_interact, every tick, 200 ms lapse) gates the hand-off. ol_interact
`OLInteract_RoomWalkStep` runs from Hook_NativePlayerMove right after OLXR_FrameLatch (and from the tick
without the latch): recenter (x, z) m -> world uu by the body yaw (as TrackToWorld), `OLUE3_HeroMove`,
step-up (30 uu) when held back over 40%. Gate `RoomWalkWhyNot`: setting, head position on, not
SceneScripted, LocomotionMode 0, Physics 1, not climbing. `[Motion] RoomscaleWalk` (menu "Walk around for
real", launcher MOVEMENT). Verified with headoff ramps on the null headset: 1 m -> 90 cm, wall blocks.

## Test 45 changes: tester build

- `[CAMADJUST]` free hold aims with the controller's AIM pose (the grip's forward runs up the handle: the lens
  pointed up). Player placement: `OLInteract_Get/SetCamAdjust` -> `OLUE3_SetCamAdjust` (g_camHold4 = -roll,
  -tilt, turn; g_camAdjOff added to the right hand's IK target while camHold > 0, in the view frame). Menu
  sliders (CAMCORDER), [Motion] CamcorderTilt/Turn/Roll/ForwardCM/UpCM/RightCM.
- `[OBJSCAN]` `OLUE3_ObjectsOfClass` serves a cache; `OLUE3_ObjScanStep` (interaction tick) walks 4000
  objects a frame for every registered class; first lookup of a class = one full scan (logged with ms).
- Ladders: `s_ladderBlockUU` 0 (no barrier); PhysClimb keeps Miles on a ladder with no hand (s_pcOnLadder)
  unless at its foot or B.
- Defaults: CamcorderLiveScreen 0 (experimental overlay), CoverMouth 0 (out of menus; OLSound_Install only
  when 1 - ReadConfig reads it).

## Test 46 changes: tester build 2

- `[DIRECTTURN]` smooth/snap turn writes yaw straight into Actor.Rotation (+0x8C, by reflection) of BOTH the
  PlayerController and the hero at the start of the player tick (`TurnApply`, before RoomWalk/LS_Tick). The
  controller alone is overwritten: Outlast copies the pawn's yaw back each tick. Only while
  `OLUE3_DirectTurnReady` (hero, move mode 0/1, no cinematic, look allowed, gameplay); otherwise the old
  stick path. Rate = sign * t^1.5 * SmoothTurnDegPerSec ([Input], 30-360, default 150), snap = one
  `OLUE3_TurnStep`. Null HMD: 1 s full stick = 148.7 deg; snap 90 = 16384 units in one frame.
- `[LIVESCREEN]` + `[LENSCAM]` the flip screen shows the lens's own view:
  - TextureRenderTarget2D.Create 352x264 (a size nothing else uses), SceneCapture2DActor spawned and moved
    to the lens pose (CamPlace) every tick with Actor.SetLocation/SetRotation, 30 fps, post-process on,
    MaxViewDistanceOverride 4000 uu. Enabled only while in hand with the screen facing the eye (600 ms).
  - **UE3 ProcessEvent rejects functions with a native index**: UFunction.iNative (WORD +0xD4; FunctionFlags
    +0xD0, FriendlyName +0xD8). SetLocation (267) / SetRotation (299) silently did nothing. `LS_Call`
    zeroes iNative around the call (game thread) and restores it.
  - d3d9 proxy `Hook_CreateTexture` (D3D9Ex only): after `OLProxy_LensExpect(w,h)` the next RENDERTARGET
    texture of that size is the lens (AddRef'd, `OLProxy_LensTex`). `LensPublish` (Present, render thread)
    StretchRects it into 3 shared D3D9Ex textures with event queries, at most every 30 ms; the bridge opens
    the newest finished one (`LensSrv`) and `FlipScreenLayer` samples it whole (falls back to the eye crop).
  - Cost on the null HMD: render thread ~3-5% while capturing. Dev: `lenssnap <name>`, `livescreen on|off`.
  - The texture-swap approach (Screen_camera / camerahud D3D pointers) is dead: Screen_camera has no GPU
    texture, CameraScreen_INST's graph is stripped. Its dev ops (ls*) remain for research.
- Zoom (`MapPadButtons`): a mostly-vertical push (|y| > 0.45 and |y| > |x|) zooms; released below
  0.25 or when x dominates. The first rule (|y| > 0.6 and > 2|x|) never fired on an ordinary push, so
  the camcorder zoom looked dead and the W-T bar never moved. `[VIEWFINDER]` fills the view (quad sized
  to the eye frustum x1.03) unless [Motion] ViewfinderFrame=1.
- Vault fix: guided pacing (`paceable`) only for climbs the hands started (`s_climbByHands`); a sprint vault
  with the left grip held (sprint) froze mid-move.
- `[ASYNCLOG]` the log is buffered and written by a thread every 200 ms, lines prefixed with seconds since
  start; `[HITCH]` lists frames over 40 ms in the PERF report.
- Launcher: Turn speed, Camcorder display size (HudScalePct), Zoom with black bars; CamcorderLiveScreen
  default 1. Outlast-VR.bat removed from the package (uninstall still removes an old copy).
- `[MIRRORFPS]` the windowed Present of the 3456x1824 backbuffer (plus the one-eye blow-up) cost the render
  thread ~1.6 ms a frame; during stereo play the monitor now gets [Render] DesktopMirrorFps (30, 0 = every
  frame) presents a second - skipped Presents flush through an event query. Null HMD uncapped: door row
  123 -> 152 fps, corridor 137 -> 180.
- `[OCCLSKIP]` with OcclForce the occlusion query boxes are not drawn (draw hooks skip everything between an
  occlusion query's Issue BEGIN/END) and GetData answers "visible" at once (no wait=TRUE stalls).
  [Render] OcclusionSkip=1. +7-12% more (door row 163 -> 182, corridor 190 -> 203); picture identical.
- Measured: post effects (DOF, SSAO, bloom, light shafts, lens flares, composite lights) change nothing -
  the render thread (2 views of draw calls) is the limit; dynamic shadows off +6%; OcclForce off +2-12%.
- Dev: `con <console command>` (PlayerController.ConsoleCommand, e.g. `scale set DynamicShadows False`;
  `scale set ScreenPercentage` kills the game), `mirrorfps N`, `occlskip 0|1`. Null-HMD perf runs: test ini
  PaceToHeadset=0 + OLEngine MaxSmoothedFrameRate=300; scratchpad measure.ps1 `Measure-OL`.

## Beta 1.1.3 changes: camcorder HUD at reference scale

- `[CAMHUD]` is the game's own fullscreen display, not a panel in front of your face. The quad used to be
  0.60 m wide at 0.45 m (the middle ~67 deg), which made every readout look huge, close and bunched up in
  the centre. It is now sized from the headset's horizontal FOV (`OLCamHud::PanelWidth`, clamped to 76-84
  deg) and parked 0.75 m out, so the reference artwork spreads to the corners: REC and the clock top-left,
  the W-T lever centred, the battery gauge top-right, the HD/AUTO/F2.4 lens markings bottom-left. The clamp
  stops the corner readouts past ~37 deg, where they would be drawn in one eye only and read as a ghost.
- The artwork is back on the reference screenshot's own 1920x1080 coordinates (`DynamicTop = 0`, the
  readout band is the top 160 rows again); the enlarged/inward layout of the previous test build is gone.
- The display sits further back, into the view instead of against the lenses (`PanelDistance` 1.15 m, FOV
  fill clamped to 56-64 deg instead of the first pass's 76-84): the quad is head-locked and mono, so it has
  no stereo disparity and its apparent distance comes only from how much of the view it fills -
  `PanelWidth` scales with `PanelDistance`, so moving the quad back on its own changes nothing on screen.
  Keeping the physical width (the reference display's ~1.37 m) and filling less puts the REC light and the
  battery gauge at ~28 deg (was ~37, then ~34, then ~30), with clear space around the readouts and still
  inside the ~40 deg both eyes fuse; the 46 px REC caption is 1.6 deg tall, still well above comfortable
  reading size. `tests/camcorder_hud_test.py` asserts the panel's half-angle stays in that band.
- The focusing brackets in the middle of the display are 60% of the reference's box (212x178 instead of
  354x298, same centre and arm thickness). They are the only bright shape with nothing behind it, so the
  reference size read as a big empty square hanging in front of the face; the brackets now subtend ~7.7 x
  6.4 deg instead of ~13.8 x 11.6. They are drawn at 175 grey instead of the artwork's 190, which read as
  pure white against the dark scenes and pulled the eye away from the picture.
- The readouts sit one step further out and higher than the reference's own coordinates - REC/clock 80 px
  left and 20 px up, the spare count and gauge 80 px right and 20 px up - because the shrinking quad put
  the display's corners nearer the middle of the view. The brackets stay exactly where they were, which
  is also what the extra 10 px of lift buys: distance from the box. The gauge's rim now ends 59 px from
  the display's edge (~30.4 deg of the panel's 31.2).
- The focusing brackets are opaque, 215 grey - white that leans grey against the dark scenes.
  Translucency is not available on this path: the compositor multiplies the layer's colour by its own
  alpha despite `XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT`, and a grey box meant to be soft came
  out black at 200/255 and still dark at 235/255. The test now asserts alpha 255 and a light grey, so
  the box cannot quietly go dark again.
- Night vision lifts the scene behind the HUD to a bright green wash, where a light grey readout has no
  edge left and vanishes. The spare count gets a 1 px black rim (the same string drawn four times, once
  per neighbour, then in grey) and the gauge a 2 px black rim just outside its grey frame, terminal
  included. Four passes, not eight: measured 0.34 ms/frame for the rim with the HUD's naive 96x72 glyph
  blit (the whole top band is 0.54 ms), and it only runs while the camcorder is up.
- `OLCamHud::ZoomUnit` normalises the zoom factor (0..1, and a 0..100 percentage build) so the W-T thumb
  can never be pinned at one end, and the thumb is drawn as a bright body in a dark outline so movement is
  visible against the track.
