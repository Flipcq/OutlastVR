# Open issues and next steps

This is the state of the project at the time the original maintainer stepped back (October 2026).
Each item says what is known so far and where to look in the code. Player logs are the best
evidence for any bug: `%LOCALAPPDATA%\OutlastVR\OutlastVR_Log.txt`, overwritten every launch.

## On `main`, not in a release yet

## Done in Beta 1.2.1 (input reliability and the Valve Index)

- **The notebook shortcuts are split three ways** (polishing pass, from player feedback):
  **left grip + Y = documents** (`ShowEvidenceMenu`, D-pad right), **left grip + X = notes**
  (`ShowRecordingMenu`, D-pad left) and **left trigger + Y = the notebook menu that holds both**
  (`ShowTabMenu`, Back). A chord never also crouches or reloads the battery: A is untouched by
  every chord (Test48), B stays silent under the grip, the trigger keeps its own analog lean, and
  the Y chords keep the shipped rule that the sprint pauses while one of them is open.
  `MapPadButtons` in `src/d3d9_proxy.cpp`, the hint table in `src/ol_prompts.cpp`,
  `tests/mouse_use_test.py`.

- **Jump / use not registering every time.** Two separate leaks, both closed:
  - **A press could fall between two game polls.** UE3 reads the fabricated pad on its own tick, and
    `ReliablePadButtons` only extends a press it has already seen - so a tap that started and ended inside a
    slow frame (the 40 ms+ frames the log records: doorways, ledges, texture uploads) never reached the game
    at all. The publisher now holds every discrete button for 150 ms in the snapshot it publishes
    (`[PADLATCH]`, `LatchPresses` in `src/ol_xr.cpp`), so at least one poll always sees it. The mouse path
    keeps the raw edges, and the recenter chord clears its own latch. `tests/pad_press_test.py`.
  - **Sprint silenced Use.** "No Use while the left grip is held" was written for the removed Beta 1.1.4
    chords (left grip + B opened the notebook); what it did in practice was swallow B of every player who
    was sprinting - i.e. everybody who ran at a door. Only a grip the mod itself is using that frame (a
    handle, a ledge, a battery - `OLProxy_SetLeftGripOwned`) silences Use now. `MapPadButtons` is untouched;
    `OLProxy_MouseUseSample` in `src/d3d9_proxy.cpp`, `tests/mouse_use_test.py`.
- **Valve Index controllers: cannot walk, and no menu** (the item from the bug list below).
  - **Walking.** The profile was never the problem: `StickCenter` was. A runtime that reports "no thumb" on
    the stick while the stick is actually held down taught the drift fix that the pushed position WAS the
    rest point, and the stick then read zero for the rest of the session. A stick past 0.45 now always
    passes, and no rest point is learned from a pushed stick (`[STICKCENTER]`, `tests/stick_center_test.py`).
  - **The menu.** `/input/system/click` is the runtime's button ("may be unavailable for application use",
    and Valve's own tracker says a click there never reaches an app), so on an Index there was no way to
    reach the pause menu or this mod's settings at all. A firm press on the **left trackpad**
    (`/input/trackpad/force`, unused until now) is the menu there. `system/click` stays bound for runtimes
    that do deliver it.
  - **A refused path cannot drop the whole profile.** One path outside a profile's allowlist makes
    `xrSuggestInteractionProfileBindings` fail for the *whole* call. Every profile is now suggested twice:
    the full set, then the set without the paths a runtime may refuse (haptics, stick touch, system button,
    trackpad force). `tests/xr_valve_bindings_test.py` also checks every bound path against the OpenXR 1.1
    interaction-profile allowlists, and pins the Oculus Touch table so the Quest bindings cannot move.

- **Streaming: the black bars at the edge of view when turning the head quickly** (SteamVR,
  Quest Link / Air Link). Not a capture bug: every PC runtime reprojects the submitted frame to
  where the head is at scan-out, and a warp can only pull black where nothing was rendered
  (Khronos frame-submission guide). Three changes, all in `src/ol_xr.cpp`:
  - **`[FOVMUTABLE]`** In Beta 1.2.1 the `[FITFRUSTUM]` decision comes from the runtime's own
    `XrViewConfigurationProperties::fovMutable` instead of its name: a runtime that discards the
    declared FOV (Meta's PC runtime - Quest Link / Air Link) gets each eye pre-warped into its own
    frustum, one that honours it (SteamVR, VDXR) gets the image as drawn. `OLR_DecideFitFrustum()`.
  - **`[STREAMEDGE]`** In fit mode the eye image spans the runtime's lens frustum while the picture
    covers only what the game rendered; the margin used to be cleared to black, and that black is
    what the warp brings into view. The blit now extends the picture's own edge over the margin
    with the same affine map, so the margin is a stretched continuation of the world instead of a
    bar. `OLR_EdgeFill()`, `[Stream] EdgeFill` (default 1). Polishing pass: the decision is
    **per axis** (an asymmetric lens leaves a wide margin on one side and none on the other; one
    narrow axis used to throw the free margin away). A skipped fill now says why in the log, four
    times at most: `[OLVR][STREAMEDGE] kept black around the picture: ...`.
  - **`[STREAMPOSE]`** The pose lead is now `[Stream] PoseLeadPeriods` (default `1`, the shipped
    behaviour). `0` removes the lead entirely; `2` suits a runtime with a deep queue over a link.
    `tests/stream_edge_fill_test.py`, `docs/STREAMING.md`.

- **Running well on SteamVR alone and on Quest Link / Air Link** (the runtime-integration half,
  separate from streaming quality). Two gaps closed:
  - **A recenter done in the headset's own menu** (SteamVR dashboard "Recenter", the Oculus menu's
    "Reset View"). `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING` was in the "log each unknown
    event once" branch, i.e. dropped: the runtime redefined its origin, this mod's baseline did not
    follow, and the world rotated/slid around the player. On SteamVR alone that dashboard recenter
    is the only recenter a player has, so it read as broken head tracking. `[REFCENTER]` now folds
    the new origin into the baseline at the `changeTime` the runtime named, which leaves every
    relative pose *identical* (the runtime moves the chair, not the asylum); a runtime that sends no
    pose falls back to a plain re-zero. `OLR_ReanchorOnSpaceChange()`.
  - **Which swapchain format the runtime prefers** is now asked (`[SWAPFMT]`) instead of assumed:
    OpenXR's guidance is to use the first colour format the runtime lists, and anything else costs a
    compositor conversion pass on every eye every frame. `OLR_PickEyeFormat()` requests the shipped
    BGRA8 sRGB wherever it is offered (every runtime this mod has run on - so no change there), falls
    back to the byte-identical BGRA8 UNORM only when it is not, and otherwise keeps the old request.
    The non-sRGB path re-encodes the gamma LUT (`SrgbFromLinear`) instead of relying on the render
    target, so the picture is identical; `[XR] RuntimeFormat=0` restores the old behaviour.
  - Both are in `src/ol_xr.cpp`; `tests/stream_edge_fill_test.py` checks the re-anchor against its
    invariance property (including the orientation transform) and that neither event nor format
    query can be dropped again.

## Done in Beta 1.1.9 (document menu enhancement)

- **A confidential document on Miles's left shoulder.** A `Document-01` StaticMesh hangs from the
  left clavicle bone, invisible and collisionless. Squeeze the left grip at the left shoulder to take
  it (it becomes visible in your hand and follows the hand like the battery), let go and it returns to
  the shoulder. Look at the held document for about 0.8 s and the **documents list** opens, then the
  document returns to his shoulder. Position/rotation tuned live in the in-game menu
  (**CONFIDENTIAL DOCUMENT**). `[DOCUMENT]` in `src/ol_interact.cpp` and `src/ol_ue3.cpp`.
- **Left grip + Y now opens the notes list** (`ShowRecordingMenu`, D-pad left), not the notebook; the
  documents list (`ShowEvidenceMenu`, D-pad right) opens by looking at the held document.
  `MapPadButtons` in `src/d3d9_proxy.cpp`.
- **Y at the face with no spare batteries buzzes twice** (held or pinned), one buzz right after the
  other, instead of a single one. `YReload` in `src/ol_interact.cpp`.
- **The reload animation hands off smoothly** from the controller IK (the arms ease into the game's
  reload instead of snapping to its start pose). `OLInteract_ReloadBlend` in `src/ol_interact.cpp`.

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
- **Valve Index controllers on a Pimax headset: cannot walk** - FIXED in Beta 1.2.1 (see
  "Done in Beta 1.2.1"). The profile was active all along; `StickCenter` was throwing the stick away when a
  runtime reported no thumb on it. The Index also gained a menu (left trackpad force) and a two-pass
  suggestion that no runtime's refusal can take the whole profile down.
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
