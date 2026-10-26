# Outlast 1 VR - camcorder launcher

VR mod made by StrikeZoneVR

`Outlast1VR.exe` replaces `Outlast-VR.bat`. It is a single borderless 1280x720 window styled as the
night-vision camcorder viewfinder. It has a tape-insert / power-on boot sequence, a REC light and timecode,
scanlines, tracking glitches, grain and a procedural asylum corridor. All sound is synthesised at runtime.
No game artwork and no audio files are shipped.

## Files

| File | Purpose |
|---|---|
| `Outlast1VR_Launcher.cs` | The launcher (C# 5, .NET Framework 4.x, WinForms + GDI+ + winmm) |
| `Outlast1VR_Pages.cs` | The VR SETTINGS and QUEST 3 CONTROLS tapes (Test 38) |
| `build_launcher.cmd` | Builds `Outlast1VR.exe` with `%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe` |
| `Outlast1VR.ico` | Exe icon. It is drawn by the launcher itself (`Outlast1VR.exe --write-icon Outlast1VR.ico`) |
| `strikezone_logo.jpg` | Credit logo. The copy next to the exe wins; a copy is also embedded as a fallback |
| `preview_*.png` | Frames rendered with `--preview` (`preview_settings.png` / `preview_controls.png`: the two tapes) |

## Build

```
build_launcher.cmd
```

## Install for players

Copy `Outlast1VR.exe` (and `strikezone_logo.jpg`) into `Outlast\Binaries\Win64`, next to `OLGame.exe`,
`d3d9.dll` and `openxr_loader.dll`. If the exe is anywhere else, it says so clearly and the install buttons stay disabled.

## Controls

Mouse, keyboard or an XInput controller. A controller is read only while the launcher has focus.

- Up/Down, W/S, Tab or the D-pad / left stick move the selection.
- Enter, Space, A or Start confirm. B jumps to EXIT.
- M or the speaker icon mutes. The setting is kept in `HKCU\Software\OutlastVR\Launcher`.
- Esc closes. Drag anywhere to move the window.
- During the boot animation, any key or click skips it.

## Buttons

- **PLAY IN VR** installs the settings, plays a heartbeat / "put on your headset" transition, starts
  `OLGame.exe -installed -seekfreeloadingpcconsole` from its folder, then closes.
- **INSTALL SETTINGS ONLY** installs the settings without starting the game.
- **UNINSTALL VR MOD** (was RESTORE ORIGINAL SETTINGS until Test45) needs a second press within 4 s. It now
  also removes outlastvr.ini, Outlast-VR.bat, the logo, our README / TESTER NOTES, and the launcher itself
  after it closes; with no saved game settings it still removes the files. It moves the `*.bak-outlastvr` backups back
  and deletes `d3d9.dll` and `openxr_loader.dll`, exactly like the .bat uninstall.
  If `OLGame.ini.bak-outlastvr` is missing, it reports "not installed" and changes nothing.
- **VR SETTINGS** (tape 02) lists the mod's main settings from `outlastvr.ini` next to the launcher: up/down picks,
  left/right (or Enter / a click) changes, and changes are written with WritePrivateProfileString (the resolution slider saves on release), so
  the comments in the file stay. Its TOOLS rows reset the in-headset how-to labels and open `%LOCALAPPDATA%\OutlastVR`.
- **QUEST 3 CONTROLS** (tape 03) draws both Touch Plus controllers (from Meta's product photos: round face, stick,
  face buttons, Menu / Meta, trigger under the face, grip on the inner side), steps through every control with its
  meaning in words, and shows the four hand gestures. Arrow keys step, a click on a label jumps to it.
- Esc or controller B goes back from a tape to the main menu.
- **EXIT** closes the launcher.

The status panel shows:
- the detected game folder
- whether `d3d9.dll` and `openxr_loader.dll` are present
- the mod build, from the `BUILD=` line of `%LOCALAPPDATA%\OutlastVR\OutlastVR_Log.txt`
- the active OpenXR runtime, from `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`, shown as Virtual Desktop, SteamVR, Meta / Oculus or Other
- whether the VR profile is installed

## What the installer does (port of release\Outlast-VR.bat)

The config folder is `[MyDocuments]\My Games\Outlast\OLGame\Config`. Documents may be redirected by OneDrive.

Before installing, the launcher refuses to continue and explains why if any of these is true:
- `OLGame.exe` is not next to the launcher
- `d3d9.dll` is missing
- Outlast is running
- `OLGame.ini` does not exist yet

Steps:

1. **Backups.** `OLGame.ini`, `OLSystemSettings.ini` and `OLEngine.ini` are copied to `*.bak-outlastvr`,
   only when no backup exists yet.
2. **OLGame.ini.** Every line matching (case-insensitive, like PowerShell `-replace`) is set as follows:
   - `DefaultFOV=120.0`
   - `RunningFOV=126.0`
   - `CamcorderMaxFOV=120.0`
   - `CamcorderNVMaxFOV=120.0`
   - `NVLightZoomedInBrightness=0.015`
   - `MinCosAngleForPickup=0.70`
   - `PickupInteractRadius=200.0`
   - `bShowCrosshair=false`
3. **OLSystemSettings.ini, `[SystemSettings]` block only:**
   - `DepthOfField=True`
   - `MotionBlur=False`
   - `MotionBlurPause=False`
   - `UpscaleScreenPercentage=False`
   - **`Fullscreen=False`**: this build's high-resolution renderer needs windowed mode. The .bat used `True`.
   - `ResX` / `ResY` set to the physical primary desktop size, ignoring Windows display scaling
   - `ScreenPercentage=100.000000`
   - `bAllowPostprocessFXAA=False`
   - `MaxAnisotropy=16`
   - on `TEXTUREGROUP_*` lines, `MipFilter=Point` becomes `MipFilter=Linear`
4. **OLEngine.ini.** Only the stock values change:
   - `MaxSmoothedFrameRate=62` (or `62.0...`) becomes `90`
   - `PoolSize=160` becomes `PoolSize=1000`

Files are written the way Windows PowerShell 5.1 `Set-Content` writes them: ANSI code page, no BOM, CRLF line
endings. A file is only rewritten if its bytes would change. On copies of real config files, the output was
byte-identical to the .bat's own PowerShell commands (with `Fullscreen=False`). Missing files are skipped with
a warning, and a failed backup aborts before anything is edited.

## Choosing the VR render resolution

In **VR SETTINGS > PICTURE**, leave **High-resolution rendering** ON and choose:

- **Eye resolution mode: AUTO** (default): OpenXR supplies the recommended size per eye.
  It includes the runtime's quality settings, so it can differ from the physical panel resolution.
  **Render resolution** scales this recommendation to trade sharpness for performance.
- **Eye resolution mode: MANUAL**: use the **Eye resolution** slider, shown as a line with a
  circular handle and `width x height` beside it. Drag left for fewer pixels or right for more.
  The dimensions update while dragging and are saved on release. Left/right arrows or the D-pad
  also adjust it. Both dimensions scale together using the aspect ratio of the saved
  `EyeWidth`/`EyeHeight` pair, with 8-pixel rounding and limits of 480-4096 per axis. Losing
  mouse capture cancels an unfinished drag. The slider is enabled when MANUAL and high-resolution
  rendering are enabled. Render resolution (%) is ignored in this mode. Restart Outlast after
  changing the mode or manual size.

For example, 1440 x 1600 means **each eye** renders at 1440 x 1600; the engine draws both
in a 2880 x 1600 frame. The OpenXR output size remains the runtime recommendation, and the
mod scales the rendered picture to it. The desktop window size is a separate setting.

The settings are saved in `[Render]` as `AutoEyeResolution` (1 = AUTO, 0 = MANUAL), `EyeWidth`
and `EyeHeight`. Existing configs without the new key keep AUTO behavior. With `HighResRender=0`,
the legacy desktop render path is used and these eye settings do not apply.

## Optional background from the user's install

If `..\..\Engine\Splash\PCConsole\Splash.bmp` (or an `OLGame\Splash` equivalent) exists in the user's own
install, the launcher reads it at runtime. It processes the image into night vision and lays it over the
corridor for about one second, roughly every 41 seconds, as a double exposure with a static cue. Nothing from
the game is saved or shipped. Without that file, the procedural corridor stands alone. It has fog, dust,
a flickering ceiling lamp and a figure with eye-shine in the far doorway.

## Developer flags

None of these flags play sound, and none of them write anything except the requested output.

- `--preview out.png [--frame N]`: renders a frame at animation time N/30 s. The default N is 150, which is after the boot.
  Extra options:
  - `--hover I` and `--hover-age S`: select a button and set how long it has been hovered
  - `--launch-frame N` and `--exit-frame N`: show the launch or exit transition
  - `--restore-armed`: show the restore confirmation state
  - `--dry-install`: read the real config read-only and show the install report
  - `--gamedir DIR`: read status from another game folder, read-only
  - `--procedural`: ignore the splash
- `--bench out.txt`: timings for 150 frames, about 13 ms per frame on an i5-13420H.
- `--ini-test DIR [--res WxH]` / `--restore-test DIR --gamedir FAKEDIR`: run the real install / restore code on
  copies.
- `--selftest out.txt`: runs a silent mixer test (muted), builds the form without showing it, and dry-runs the menu actions
  (including opening and leaving both tapes).
- `--page 1|2 [--page-sel I] [--page-age S]` with `--preview`: render the settings / controls tape.
- `--settings-test DIR`: the real settings load / change / save path against `DIR\outlastvr.ini` (use a copy).
- `--dump-audio DIR`, `--write-icon file.ico`, `--sysinfo out.txt`.
