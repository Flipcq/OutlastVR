# Outlast VR - technical overview

*How the mod works inside. For players, see the [main README](../README.md).*

A 6DOF VR mod for Outlast (2013), built as a `d3d9.dll` proxy loaded next to `OLGame.exe` -
no game-provided modding API exists for this engine build, so the mod hooks Direct3D 9
directly and reads UE3's own object/reflection tables at runtime to find what it needs,
rather than relying on an SDK.

⚠️ Experimental community mod. Not affiliated with Red Barrels. Use at your own risk.

Licensed under [GPL-3.0](../LICENSE).

## What it does

- **Real geometric stereo**, not reprojection - the engine renders both eyes side-by-side
  in one frame (`ULocalPlayer::CalcSceneView` called twice with the viewport rect and eye
  offset rewritten), which the proxy then splits and submits per eye. Getting this stable
  required finding and working around a D3D9 hardware-occlusion-query bug: the query pool
  is built for one view per frame, so a second view read back another query's stale result
  and skipped meshes that were plainly on screen.
- **6DOF head tracking** - position and rotation from OpenXR are injected into the game's
  own view-point hook, gated so only the render pass is affected (AI/aim/audio keep the
  gamepad's answer). Declared FOV to the compositor always matches what the engine actually
  rendered.
- **Motion controllers as a virtual gamepad** - an OpenXR action set is merged into the
  XInput state the game polls, so the game plays as normal with real controllers, glyphs
  included, no physical pad required.
- **Gameplay upper-body IK** - both arms follow their matching VR controllers during ordinary
  play, while menus, cinematics, camcorder zoom and native interactions retain authored arm
  animation ownership.
- **Cutscene and menu handling** - narrow-FOV renders (menus, some cutscenes) fall back to
  a world-locked cinema screen rather than stretching a narrow frame across the whole
  headset; some cutscene cameras render at full width like gameplay instead.
- **Color/gamma correction** - the game's own brightness calibration and its ~2.2-gamma
  authoring are reconciled with the sRGB frame the compositor expects.

## Experimental areas

- **Hand/arm IK.** Position, elbow targets and controller orientation are driven through
  UE3's native SkelControl system. It is enabled for gameplay testing and deliberately yields
  during authored interactions and cinematic cameras.

## Building

See [BUILD.md](../BUILD.md). Short version: Visual Studio 2022 with the C++ workload, then
`scripts\build.bat`. All dependencies are vendored in `third_party/` - nothing else to
install.

## Layout

| Path | What's there |
|---|---|
| `src/` | Mod source (the D3D9 proxy, UE3 hooks, OpenXR layer, ImGui menu) |
| `scripts/` | `build.bat` |
| `launcher/` | The `Outlast1VR.exe` launcher (C#, built by `launcher\build_launcher.cmd`) |
| `third_party/` | Vendored MinHook and Dear ImGui, used by the build |
| `docs/` | Recon and research notes; `docs/dev` has the Beta 1.0 developer change log |
| `release/` | The player README and the controls pictures shipped in the release zip |
| `runtime/` | A starting `outlastvr.ini` |
| `builds/` | Build output (not in the repo - download the release zip instead) |

## License

GPL-3.0 - see [LICENSE](../LICENSE). Copyright (C) 2026 the Outlast VR authors.

Vendored dependencies keep their own licenses: MinHook (BSD-2-Clause) and Dear ImGui (MIT),
both under `third_party/` alongside their license files. Both are permissive licenses
compatible with GPL-3.0.
