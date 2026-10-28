# Open issues and next steps

This is the state of the project at the time the original maintainer stepped back (October 2026).
Each item says what is known so far and where to look in the code. Player logs are the best
evidence for any bug: `%LOCALAPPDATA%\OutlastVR\OutlastVR_Log.txt`, overwritten every launch.

## On `main`, not in a release yet

- **Head bob fully off while running (Beta 1.1.1).** In Beta 1.1, running strides still swung the
  view by about 13-14 cm, just under the 14 cm "that was a real move" limit, so they passed through.
  Plain walking and running now take out up to 45 cm and 15 deg of sway and up to 30 cm of bounce.
  Jumps, vaults and landings fade the steadying out over 0.25 s. This is `[BOB2]` in
  `src/ol_ue3.cpp` (`BobStabilize`, `BobCorrection`). It has **not been tested in a headset yet**.
  The `[BOB2]` log line every 5 s shows what was taken out.

## Done in Beta 1.1.4 (camcorder enhancement)

- **GAME POSE is upright.** The camcorder's base pose no longer inherits the raise animation's
  12 deg roll and 60 deg turn, and its lens follows the way you face instead of the hand's sideways
  position. `g_camHold3` in `src/ol_ue3.cpp`.
- **Hold the right stick click** for about 0.8 s: Miles raises the camcorder with the game's own
  animation and it stays at his face with your hand resting. Hold it again, or squeeze the right
  grip at your face, to lower it. A short click is still night vision. `[FACECAM]` in
  `src/ol_interact.cpp`, `src/ol_ue3.cpp` and `src/d3d9_proxy.cpp`.
- **A waist-shaped camcorder grab zone**, wider down and forward so it also covers a hand resting
  in the lap when you play seated. `kWaist*` in `src/ol_interact.cpp`.
- **Left grip + B and left grip + X are removed** (they opened the notebook / recordings by
  accident while sprinting). Left grip + Y is still documents.

## Bugs players reported

- **Spotlights bleed through walls in the sewers.** Not investigated yet. The mod does not change
  the game's shadow settings, so first compare with the mod off at the same spot. Two of the mod's
  render changes are suspects:
  - `[OCCLSKIP]` / `OcclForce` (`src/d3d9_proxy.cpp`, `outlastvr.ini`): every occlusion query
    answers "visible".
  - `[LENSFIT]` (`src/ol_ue3.cpp`): each eye has its own off-centre frustum. This already cut the
    night-vision lamp's light once (`[NVEYES]`), so the light's screen bounds may be computed for
    a centred view.
- **Valve Index controllers on a Pimax headset: cannot walk.** The arms move and the camcorder can
  be grabbed, but there is no stick movement. Known so far:
  - The `valve/index_controller` bindings in `src/ol_xr.cpp` include the thumbstick and its touch
    sensor.
  - The working grab means the Index profile is active.
  - The log names the interaction profile per hand, and the `[STICKCENTER]` lines show the raw and
    sent stick values. That is the first thing to read.

  Ideas:
  - also bind the Index trackpad to walking;
  - retry the bindings without the optional paths (`system/click`, haptics, touch) if a runtime
    rejects the whole profile;
  - make sure `[STICKCENTER]` can never zero a healthy stick (`[Input] StickAutoCenter=0` turns it
    off for a test).
- **Depth feels "a little off"**, and the **World size slider "messes with the IPD"**. World size
  (`[WORLDSIZE]`, `src/ol_ue3.cpp`) scales the eye distance and head movement together. That is how
  world scale works, but it changes the eye distance you feel. The eyes otherwise use the
  runtime's IPD.

  Ideas:
  - a separate eye-distance fine-tune that only changes depth;
  - show the detected IPD in the menu;
  - ask players which headset they use, and whether the world looks too big or too small.
- **Slight double vision** for some players. No clear repro.
- **A "giraffe arm"** (stretched arm) in one scripted scene. No repro.
- **Doors can push Miles** when they close on him.
- **The "real camcorder" hold** (`CamcorderHold=5`, the aim pose turned 180 deg) is broken. It is
  out of the menus, the default is 4 (free hold), and dev command `camhold 5` still selects it.
- **The live flip screen** (`CamcorderLiveScreen`, a SceneCapture2D lens camera) is experimental
  and off. Showing the picture on the camcorder's own screen texture is not done.
- **Not verified on real ledges:**
  - climbing pacing and auto ledge grab (`[CLIMB]`, `[PHYSCLIMB]`, `[AUTOLEDGE]`);
  - only the scaffold ladder in the opening has been tested.

## Requested features

- **Running and jumping that players can find.** Sprint is a left stick click, or left grip while
  walking. Vault and jump is A while moving. Several players did not know this.
- **More headsets and controllers tested:** Pimax, Index, Vive, WMR and Pico. The bindings are all
  in `src/ol_xr.cpp`.
