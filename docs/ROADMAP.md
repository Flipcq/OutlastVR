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

- **Keep the camcorder in hand without holding the grip.** For example, a "camcorder stays in hand"
  option: squeeze at the hip to take it or to put it back. The holster state machine is `Holster()`
  in `src/ol_interact.cpp`. `[Motion] CamcorderControl=1` (grip toggles the game's camcorder) already
  exists as an accessibility mode.
- **A bigger camcorder grab zone for seated play.** The hip zone is `kHolsterOff` (20 cm right,
  60 cm below the eyes) with `kHolsterReach` = 16 cm, in `src/ol_interact.cpp`. Ideas: about 26 cm,
  and a "Holster: Hip / Chest" option. The very first version had the holster on the chest.
- **Running and jumping that players can find.** Sprint is a left stick click, or left grip while
  walking. Vault and jump is A while moving. Several players did not know this.
- **More headsets and controllers tested:** Pimax, Index, Vive, WMR and Pico. The bindings are all
  in `src/ol_xr.cpp`.
