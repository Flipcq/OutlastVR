# Contributing to Outlast VR

**Outlast VR is open for anyone to work on.** The original maintainer has stepped back. Fixes,
features, forks and new releases from the community are welcome. The code is GPL-3.0, so
anything you build on it stays open source under the same license.

- The open bugs and requests, with what is already known about each one, are in
  [docs/ROADMAP.md](docs/ROADMAP.md). That is the best place to start.
- Use [issues](../../issues) for bugs and ideas, and pull requests for changes.

## Building

You need Windows 10/11 x64 and Visual Studio 2022 with the "Desktop development with C++" workload.

| What | How | Output |
|---|---|---|
| The mod | `scripts\build.bat` | `builds\d3d9.dll` |
| The launcher | `launcher\build_launcher.cmd` (uses the .NET Framework 4 `csc.exe` that ships with Windows) | `launcher\Outlast1VR.exe` |

To run it, copy `d3d9.dll`, `openxr_loader.dll` (the official Khronos x64 loader) and
`runtime/outlastvr.ini` into `<Outlast>\Binaries\Win64\`, next to `OLGame.exe`. See [BUILD.md](BUILD.md) for the details.

The mod only works with the Steam build of Outlast that its engine addresses were read from. The
`[Stage3]` block in `outlastvr.ini` checks the exe's bytes before it attaches anything. If the
game updates, the mod switches itself off rather than writing into the wrong place.

## How it works, in one minute

Outlast is a 2013 Unreal Engine 3 game on Direct3D 9. The mod is a `d3d9.dll` proxy that the game
loads in place of the real one. From there it:

1. **Hooks the engine** (`src/ol_ue3.cpp`). It renders every frame twice, once per eye, through the
   game's own `CalcSceneView`. It writes your head pose into the camera and solves Miles's arms to
   your controllers.
2. **Gets the picture to the headset** (`src/d3d9_proxy.cpp`, `src/ol_xr.cpp`). D3D9Ex shares each
   frame with a D3D11 device, which submits it to OpenXR. It works with SteamVR, Virtual Desktop and
   Meta's runtime.
3. **Turns your controllers into the game's gamepad** (`src/ol_motion.cpp`, the XInput hook in
   `d3d9_proxy.cpp`). The game still thinks it is reading a controller.
4. **Adds the hands-on parts** (`src/ol_interact.cpp`): doors, the hip holster, raising the
   camcorder to your eye, the hand battery reload, climbing, and the how-to labels.

[docs/TECHNICAL.md](docs/TECHNICAL.md) has the longer version. The reverse-engineered engine
offsets are listed in `docs/dev/PROJECT-BASELINE.md` and next to the code that uses them.

## Code map

| File | What is in it |
|---|---|
| `src/d3d9_proxy.cpp` | The proxy and its D3D9Ex translation. Also: the texture-streaming fix (`[TEXSHADOW]`), occlusion-query handling (`[OCCLSKIP]`), the XInput hook, the log, and saving the ini. |
| `src/ol_xr.cpp` | The OpenXR session, swapchains and frame timing. Controller actions and interaction-profile bindings (Touch, Index, WMR, Vive, simple). Recenter, the head-locked overlays (battery gauge, labels) and the cinema screen for menus. |
| `src/ol_ue3.cpp` | The engine hooks: stereo views, per-eye lens frustums (`[LENSFIT]`), head tracking, world size, head-bob removal (`[BOB2]`), arm IK and arm fixes, body follow and turn, the camcorder model and its hold modes, climbing. |
| `src/ol_interact.cpp` | Doors, holster, raise-to-eye, battery reload, climbing grabs, markers, how-to labels. |
| `src/ol_motion.cpp` | Turning, walk direction, crouch, stick pitch. |
| `src/ol_menu.cpp` | The in-headset ImGui settings menu (INSERT, or left grip + Menu). |
| `src/ol_prompts.cpp` | Rewrites the game's button hints into VR controller words. |
| `src/ol_sound.cpp` | Wwise hooks. Only used by the optional cover-your-mouth feature. |
| `src/ol_devbridge.cpp` | The developer test harness (see below). |
| `launcher/` | The found-footage launcher: installer, uninstaller, VR SETTINGS and CONTROLS pages. |
| `runtime/outlastvr.ini` | Every setting with its default, and the engine addresses. |

## Testing without wearing a headset

Most of this mod was built and tested on a PC where the developer could not wear the headset:

- **DevBridge.** Set `[Debug] DevBridge=1` in `outlastvr.ini`. Commands written to
  `%LOCALAPPDATA%\OutlastVR\dev\cmd.txt` let you screenshot the game (`snap`, `xrsnap`), press pad
  buttons, fake both controllers (`hands 1`), wave the head, and read or write engine objects through
  UE3 reflection. The command list is at the top of `src/ol_devbridge.cpp`.
- **SteamVR's null headset** gives a real OpenXR session with no headset. Enable the `null`
  driver and set `forcedDriver=null` in SteamVR's settings. Point the mod at it per process with
  `[Debug] XrRuntimeJson=<SteamVR>\steamxr_win64.json`, so your system OpenXR runtime stays as it is.
  Pitfalls:
  - With no room setup, SteamVR opens Room Setup and closes the game. Create a standing chaperone
    first.
  - Standby stops rendering, which makes the numbers meaningless. Set a long screen-off timeout and
    `pauseCompositorOnStandby=false`.
  - Launching through Steam while SteamVR is already running can hang on a hidden VR prompt.
  - The title screen only takes input while the game window has focus.
- **The log.** Everything is in `%LOCALAPPDATA%\OutlastVR\OutlastVR_Log.txt`, with a `[TAG]` per
  feature. It is overwritten every launch. When players report a bug, ask for this file, and ask
  them to send it before they start the game again.

## House rules

- **Never commit game files.** That means anything extracted from Outlast: textures, sounds, maps,
  the exe. Players must own the game. The launcher draws and synthesises everything it shows.
- **Put every new behaviour behind an ini setting** where it could go wrong. Keep the old path one
  setting away, so a player can switch a feature off without reinstalling.
- **Change one mechanic at a time.** This mod is a stack of engine hooks. Most regressions came from
  piling experiments on top of unverified ones.
- **Tag your log lines** (`[OLVR][YOURTAG] ...`) so a player's log tells you what happened.
- **Engine offsets** are fixed for the Steam build. Write down where each one came from, next to
  the code that uses it.

## Releases

The binary releases on this repository are zips of:
- `d3d9.dll`, `openxr_loader.dll`, `outlastvr.ini` and `Outlast1VR.exe`;
- `README.txt` and the controls cards in `release/controls`;
- `LICENSE.txt` and the third-party notices.

Before zipping, make sure the shipped `outlastvr.ini` has no `[Debug]` keys.
`release/README.txt` is the player-facing readme that goes in the zip. Forks are free to publish
their own builds under the GPL.
