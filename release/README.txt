================================================================================
  OUTLAST VR - BETA 1.1
  VR mod made by StrikeZoneVR
================================================================================

Outlast in VR with full head and hand tracking: look and lean with your body,
see Miles's arms move with yours, hold the camcorder in your hand, raise it to
your eye to film, push heavy doors open with your hands and climb ledges and
ladders hand over hand.

This is a BETA. Please report anything odd (see REPORTING A PROBLEM below).

--------------------------------------------------------------------------------
WHAT YOU NEED
--------------------------------------------------------------------------------

  - Outlast on Steam (the normal 64-bit game, Binaries\Win64\OLGame.exe).
  - A Meta Quest 3 with Virtual Desktop (made and tested this way), Quest
    Link / Air Link (fixed in Beta 1.1), or SteamVR. Other OpenXR headsets
    and runtimes may work but are not tested.
  - Run Outlast once normally first, so the game creates its settings files.

--------------------------------------------------------------------------------
INSTALLING
--------------------------------------------------------------------------------

  1. Find your Outlast folder: in Steam, right click Outlast -> Manage ->
     Browse local files, then open  Binaries\Win64.

  2. Copy ALL of these files into that folder, next to OLGame.exe:

         Outlast1VR.exe          the launcher
         d3d9.dll
         openxr_loader.dll
         outlastvr.ini
         strikezone_logo.jpg

  3. Start Virtual Desktop and connect the headset. Run Outlast1VR.exe and
     choose PLAY IN VR. It applies the VR settings and starts the game.

     Run it again after Steam updates or verifies Outlast (Steam can reset
     the game's settings files).

  The launcher also has:
     VR SETTINGS        turning, comfort, the hands-on controls, picture -
                        saved the moment you change them
     QUEST 3 CONTROLS   both controllers with every button explained, and the
                        hand gestures

--------------------------------------------------------------------------------
FIRST MINUTES IN VR
--------------------------------------------------------------------------------

  - Sit or stand comfortably and hold still for a second: that becomes your
    centre. Press both stick clicks together any time to recenter.
  - Small labels in the headset show you the hand controls the first few times
    (they stop once you have done each one 3 times).
  - If you shift your seat or sit back, the body follows you after a few
    seconds - you rarely need to recenter.
  - Walk around your room for real and Miles walks with you, step for step.
    Walls and shut doors stop him the way they stop the stick, and he steps
    up low steps. Small leans stay leans. Only while he is on his feet in
    ordinary play (not on a ladder, climbing, in a locker or a cutscene).
    (VR SETTINGS: "Walk around for real".) Mind your real walls: the
    headset's boundary still keeps you safe, the game's walls do not.

--------------------------------------------------------------------------------
QUEST 3 CONTROLS
--------------------------------------------------------------------------------

  LEFT CONTROLLER                          RIGHT CONTROLLER
  Stick: walk                              Stick: turn (smooth or snap,
                                             at the speed you set)
  Stick click: sprint                      Stick click: night vision;
                                             hold it: raise / lower the
                                             camcorder at your face
  Grip (hold): sprint; on a ledge or       Grip at your right hip: take the
    a handle: hold on                        camcorder (see HANDS below);
  X: crouch / drop                          on a ledge or a handle: hold on
  Y: reload the camcorder battery          A: jump, climb, vault
  Trigger: lean left / look back           B: use / open / close / pick up
  Menu: pause                              Trigger: lean right / look back
                                           Meta button: the headset's menu
  Left grip + Y: documents                 Both stick clicks: recenter
  Left grip + Menu: VR settings menu

  With the camcorder up: push the right stick straight up / down to zoom (a
  sideways push only turns). The zoomed picture fills your view; VR SETTINGS
  "Zoom with black bars" puts it in a framed window instead.

  In the game's menus (pause, notebook, notes): the left stick moves, A picks,
  B goes back.

  During gameplay, B uses the left mouse input. Keep interaction bound to Left
  Mouse Button in the game's settings. Menu controls and the remaining left grip
  shortcut keep their original bindings. Set [Input] MouseUse=0 in outlastvr.ini to
  restore X for use and B for crouch. The control images show the old layout.

--------------------------------------------------------------------------------
HANDS
--------------------------------------------------------------------------------

  THE CAMCORDER
    It hangs on Miles's right hip. Reach your right hand to your right hip
    (or to your lap, if you play seated) and squeeze the grip: the camcorder
    glides into your hand. It moves and turns
    exactly with your controller, like holding a real one - the lens points
    where your controller points. To place it differently in your hand: VR
    SETTINGS in the game (left grip + Menu) > CAMCORDER: tilt, turn, roll,
    forward, up, right.
    Bring it up to your eye to look through it (filming, night vision, zoom);
    take it away from your eye and it is back in your hand. Or hold the right
    stick click for a moment: Miles raises it with the game's own animation and
    it stays at his face with your hand resting. Hold the right stick click
    again, or squeeze the right grip at your face, to lower it again. A short
    click is still the night-vision switch. Open your hand anywhere and it goes
    back to your hip. (VR SETTINGS: "Camcorder in your hand" GAME POSE holds it
    upright in front of you with the lens ahead, the way Miles raises it.)
    Reload: press Y. (A hands-on reload is in VR SETTINGS: "Reload with your
    hands".)
    "Custom camcorder HUD" shows REC/time, W-T zoom, spare batteries/capacity
    and charge using the game's values. It replaces the large battery gauge
    and hides the original camcorder display. Switch it off to restore it.
    Playing seated, or your arm gets tired? VR SETTINGS -> "Camcorder
    controls" GRIP BUTTON: squeeze the right grip once to raise the camcorder
    and again to lower it. You look through it with your head, so your hand
    can rest anywhere. (The right grip then does not grab doors or edges -
    use your left hand or B.)

  DOORS
    Doors are heavy. Push one with your open hand and it swings as far as you
    push: a slow push creeps it open, a shove swings it on a little before it
    slows. Or put your hand on the handle, squeeze the grip and swing it open
    or shut - let go mid-swing and it carries on. Locked doors buzz. B still
    opens doors in one press. A door you open with your hands is opened for
    the game too: whatever waits behind it happens, just as with B.

  CLIMBING
    Your hands carry Miles. Squeeze a grip on an edge you can climb or on a
    ladder rung and that hand holds on: pull it down and Miles rises with it,
    pull it in and he comes closer, move it sideways and he shimmies along.
    Hand over hand: take a new hold higher up, let go of the old one. Get the
    edge to your chest and he hauls himself over onto the top. At the top of
    a ladder, keep pulling and he steps off onto the floor. Let go of
    everything and he drops - except on a ladder: let go part-way up and Miles
    holds on until you take a rung again (X drops him). Walk right up to a
    ladder and take its rungs, or walk into it and the game climbs it its own
    way. A still climbs the game's way.
    (VR SETTINGS: "Climbing" GUIDED = the game's own climb, played only as
    fast as you pull; A BUTTON = the old way.)
    Vaulting: run at a low wall or a table and press A - Miles vaults it at
    the game's own speed, whatever your hands are doing.

  JUMPING
    Your arms stay yours while Miles jumps, falls, lands and vaults.

  The game's own hints name these controls ("Grab the handle to open, or
  press B", "hold down the left grip") instead of Xbox buttons.

--------------------------------------------------------------------------------
SETTINGS
--------------------------------------------------------------------------------

  The launcher opens full screen. F11 (or Alt+Enter) switches it between
  full screen and a window on the monitor it is on. It remembers.
  In the launcher: VR SETTINGS. In the game: hold the left grip and press Menu
  (or press INSERT on the keyboard). Everything is also in outlastvr.ini, next
  to OLGame.exe, with an explanation above each setting.

  Comfort favourites: Turning SMOOTH or SNAP, "Smooth turn speed" (60
  degrees a second up to MAX, the game's fastest), snap angle, "Blink while
  snapping", "Reduce head bob" (100% = none, walking or running), "Keep the
  horizon level", "Crouch when you duck" (crouch for real), "Walk around for
  real" (roomscale), "Body turns with you" (turn your body or chair and Miles
  turns to match after a moment - no recentering needed for that).

  "World size": 100% = true size, with your own eye spacing read from the
  headset. Things look too big or "zoomed in"? Lower it. Too small? Raise it.

  "Stick dead zone" (30% by default, up to 60%): how far a thumbstick must move
  before it counts. Raise it if Miles walks, turns, or drifts to one side by
  himself - or veers left while you walk straight. Anything under it is ignored
  on each axis, so a stick that reads a little sideways while you push it
  forward still walks straight. 0% = the game's own dead zone.

--------------------------------------------------------------------------------
IF SOMETHING LOOKS WRONG
--------------------------------------------------------------------------------

  The world or body feels offset      Click both thumbsticks (recenter).
  Things look too big / zoomed in     VR SETTINGS -> "World size" 85-95%.
  The picture looks grainy/blurry     VR SETTINGS -> "Film grain" off.
  It stutters                         VR SETTINGS -> "Monitor picture" 30 FPS
                                      (not EVERY FRAME); Render resolution
                                      85-90%.
  Textures look wrong, or a crash     outlastvr.ini -> [XR] D3D9Ex=0 (a slower
                                      but very compatible path).
  The picture looks off-centre or     outlastvr.ini -> [Render] LensFit=0.
  each eye shows a distorted screen   (Quest Link sets this by itself.)
  The camcorder, doors, climbing or   VR SETTINGS -> HANDS: switch any of them
  labels get in the way               off (Camcorder on your hip off = raise it
                                      by lifting your hand to your eye, or the
                                      right grip toggles it; Climbing A BUTTON
                                      = the game's own climb).

--------------------------------------------------------------------------------
REPORTING A PROBLEM
--------------------------------------------------------------------------------

  Close the game, then send:
    %LOCALAPPDATA%\OutlastVR\OutlastVR_Log.txt
  (the launcher's VR SETTINGS -> "Open the log folder" opens it), plus a short
  video or a description of what happened and where in the game.

--------------------------------------------------------------------------------
UNINSTALLING
--------------------------------------------------------------------------------

  Close Outlast, run Outlast1VR.exe and choose UNINSTALL VR MOD (press it
  twice). It puts the game's own settings back and removes every mod file
  from the game folder; the launcher deletes itself when you close it. The
  normal game is back.

  Outlast is a game by Red Barrels. This is an unofficial fan mod.
