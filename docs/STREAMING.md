# Streaming, runtimes and the black bars at the edge of view

*Why this mod behaves differently on Virtual Desktop, on SteamVR alone and on Quest Link / Air
Link, and what the "black vertical bars when I turn my head quickly" actually are. For players;
the settings live in `[Stream]` in `outlastvr.ini`.*

---

## 1. What the mod actually submits

The game renders one frame per eye with D3D9. The proxy copies each eye into an OpenXR swapchain
image and hands the runtime **one projection layer**, twice per frame: a declared `XrPosef` (where
the head was when the picture was drawn), a declared `XrFovf` (the angle the picture covers) and
the rectangle of the image it lives in.

Everything the headset does afterwards happens **in the runtime's compositor**, on the headset or
on the PC depending on the runtime. The mod has no further say.

## 2. Why the runtimes are not interchangeable

OpenXR lets an application declare whatever pose and FOV it wants, but it only *suggests* them to
the runtime:

> "However, applications may submit an `XrCompositionLayerProjectionView` which has a different
> view or FOV than that from `xrLocateViews`. In this case, the runtime will map the view and FOV
> to the system display appropriately."
> — OpenXR specification, quoted in the Khronos forum thread
> [Oculus runtime ignores Projection Layer View's pose](https://community.khronos.org/t/oculus-runtime-ignores-projection-layer-views-pose/110078)

Whether that mapping honours the declaration is reported per view configuration as
`XrViewConfigurationProperties::fovMutable`. Three real-world answers:

| Runtime | `fovMutable` | What it does with the mod's layer | Consequence |
|---|---|---|---|
| **Virtual Desktop (VDXR)** | true | honours the declared pose and FOV, supports quad and cylinder layers ([VDXR developer notes](https://github.com/mbucchia/VirtualDesktop-OpenXR/wiki/Developers)) | the mod's own symmetric ~88-100 deg render is shown as declared - this is what every build was tuned on |
| **SteamVR** | true | honours the FOV, but a projection view declared **wider** than its own per-eye frustum is rejected and the compositor shows nothing; its scene state machine also needs a projection layer under any quad/overlay frame | works, but every frame has to be cropped to the intersection with SteamVR's frustum, and quad-only frames (menus, cinema screen) need the black placeholder layer |
| **Meta PC runtime (Quest Link / Air Link)** | **false** | discards the declared FOV **and** the per-view pose; it composites the submitted rectangle into its own asymmetric lens frustum (Quest 3 through Link: -54/+40 deg, +44/-55 deg) | a symmetric render gets stretched/displaced per eye (the "double vision" and "a screen on each eye" reports), so the picture has to be pre-warped into the runtime's frustum before it is submitted |

Both quirks are already handled in `src/ol_xr.cpp`, and Beta 1.2.1 asks the runtime instead of
guessing from its name: `[FOVMUTABLE]` reads `fovMutable` and feeds `OLR_DecideFitFrustum()`, which
turns `[FITFRUSTUM]` on exactly for the runtimes that throw the declaration away. Any runtime that
has never been tested now gets the correct treatment from its own answer.

What is left in the log tells you which path ran:

```
[OLVR][XRRUNTIME] name='SteamVR' metaFovCrop=0 steamVrFovCrop=1
[OLVR][FOVMUTABLE] the runtime says the projection FOV is mutable (a declared FOV is honoured)
[OLVR][FITFRUSTUM] off (auto, by the runtime's fovMutable flag), perEyePose=off
```

## 3. The black vertical bars during a fast head turn

They are not a bug in the mod's capture. Every PC VR runtime **reprojects** the frame it was given:
between the moment the game drew the picture and the moment the panel is lit, the head moved, so
the compositor warps the image to the latest pose at scan-out (SteamVR "reprojection"/motion
smoothing, Meta ASW, VD's SSW). A warp can shift and rotate pixels; it cannot invent what was never
drawn. From the Khronos [frame submission guide](https://github.com/KhronosGroup/OpenXR-Guide/blob/main/chapters/frame_submission.md):

> "If the app is rendering for the same pose that it submits with `xrEndFrame`, the XR system will
> be able to compensate by reprojecting the content in the correct place in the world with **just
> black pulled in on the sides**."

Turn faster or drop frames and the black crescent grows; stop and it closes again. The same report
appears for every VR app that runs under its headset's refresh rate, e.g. the
[MSFS/Reverb G2 thread](https://forums.flightsimulator.com/t/vr-black-flickering-borders-in-reverb-g2-when-turning-head/362184)
where the borders vanish when the frame rate goes up, and it is why
[ALVR was asked to render a wider FOV than the headset needs](https://github.com/alvr-org/ALVR/issues/1656)
in streaming setups.

Three things make it worse here, in order of how much they matter:

1. **The game presents below the headset's refresh.** Outlast is a 2013 UE3 title: 50-70 fps is
   normal, while a Quest 3 runs 72/90/120 Hz. Every second displayed frame is then the runtime's
   own synthesis, and each synthesis is a warp of up to a full frame of head motion. This is the
   dominant factor.
2. **Nothing outside the submitted frustum exists.** With the render matching the lens exactly
   (which is what `[Render] WideFov=1` does) there is *no* margin for the warp to pull from, so the
   first degree of unexpected motion is already black. Real engines keep a few degrees of rendered
   overscan for exactly this; OpenXR has no way to ask for it, so the margin inside the eye image
   is the only place it can live.
3. **The link itself.** Air Link/VD encode, ship and decode every frame: 20-40 ms on top of
   everything else, and the pose has to be predicted further ahead. On Quest Link the runtime also
   has `ASW`, which [halves the frame rate and synthesises the missing frames](https://developers.meta.com/horizon/documentation/native/pc/asynchronous-spacewarp/)
   when the GPU cannot keep up: ASW is the single most visible producer of edge artifacts on Link.

## 4. What this mod does about it (Beta 1.2.1)

- **`[Stream] EdgeFill=1`** - in `[FITFRUSTUM]` mode the eye image spans the runtime's lens
  frustum while the picture only covers the part the game actually rendered; the rest used to be
  cleared to black, and *that* black is what the warp smears into view. The blit now extends the
  picture's own edge texels over that margin (the same affine map, sampled with a clamping
  sampler), so the margin is a stretched continuation of the world instead of a black bar. The
  visible picture stays exactly where it was - only the previously-black margin changes. A picture
  that covers less than three quarters of the eye **on that axis** (a menu, a deliberately narrow
  camera) keeps its black field on that axis: stretching that far would be a smear, not an overscan
  margin. The decision is per axis because an asymmetric lens (Quest 3 through Link: -54/+40 deg)
  leaves a wide margin on one side and none on the other, and testing both axes together threw the
  free one away. `OLR_EdgeFill()`.
- **`[Stream] EdgeFillMargin=4`** - the margin that *cannot* be extended, because nothing was ever
  rendered past the lens edge on that side, is bought instead: the picture is drawn 4 % smaller
  (centred) inside the eye image and the fill covers what is left, so 2 % of the lens on every side
  is a band of the picture for the warp to pull from. The cost is the same 4 % of field of view
  (the world reads 4 % smaller - it zooms out slightly). `0` = off, the picture keeps its exact
  size; `8-15` = more margin, more FOV given up. This is the knob for "the bars are still there at
  the corners on a fast turn". **It is not a fit-mode setting**: on SteamVR or Virtual Desktop the
  fit never runs and the eye image *is* the picture, so there is no margin anywhere either - the
  same margin is taken off the picture in the plain blit path (`BlitHalfToEye`, and the held frames
  in `BlitHoldToEye`), with the declared frustum untouched. The theater screen and the camcorder
  viewfinder are quads, not the immersive view, and are left alone.
- **A diagnosis in the log when the fill is skipped**: `[OLVR][STREAMEDGE] kept black around the
  picture: it covers 58% of the eye horizontally and 55% vertically ...`, four times at most. If
  the bars are back, that line says whether the picture was too narrow for the fill or something
  else is going on.
- **`[Stream] PoseLeadPeriods`** - how many display periods ahead of the compositor's display time
  the pose handed to the camera is predicted. `1` is what every released build does. `2` suits a
  runtime with a deep queue over a link (Air Link at high bitrate); `0` removes the lead entirely,
  for A/B testing a "the world lags behind my head" report.
- **`[FOVMUTABLE]`** - the fit decision described in section 2, now taken from the runtime's own
  answer instead of its name.
- **A diagnosis in the log.** When the game delivers well under the headset's refresh, the mod says
  so once every 30 s, five times at most, with the numbers and the setting that fixes it:
  `[OLVR][STREAM] the game is handing over 58 fps to a 90 Hz headset, so most displayed frames are the runtime's own reprojection ...`.

## 5. What the player can do (biggest win first)

| Action | Why |
|---|---|
| Set the headset refresh to what the game can actually hold - 72 Hz, or 60/72 on a weak GPU | removes the synthesised frames that produce the bars at all |
| Quest Link / Air Link: turn **ASW off** (Oculus Debug Tool, "Asynchronous SpaceWarp" = Disabled) | ASW's synthesised frames are the artifact's worst case |
| SteamVR: disable **Motion Smoothing** for this app | the same mechanism, SteamVR's own version |
| Lower the eye resolution (`VR SETTINGS > PICTURE`, AUTO 85-90 % or a MANUAL size) | the game renders less per eye and holds a higher frame rate |
| Wireless: 5 GHz / Wi-Fi 6 AP in the same room, PC on cable; Air Link bitrate 100-150 Mbps, Virtual Desktop 60-90 Mbps | every millisecond on the link is extra warp |
| Keep `[Stream] EdgeFill=1` | what is left of the effect becomes a slight smear instead of a black bar |
| Raise `[Stream] EdgeFillMargin` (4 -> 8) if the corners still flash black on the fastest turns | trades that much field of view (the world reads that much smaller) for more of the picture for the warp to pull from |

## 6. When the runtime recenters itself (SteamVR "Recenter", the Oculus menu's "Reset View")

This is the other half of "it does not work well on SteamVR alone / on Link", and it has nothing to
do with streaming quality. Both runtimes let a player move the *origin of the play space* from
their own menu. OpenXR reports that as `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`, with
`poseInPreviousSpace` = the pose of the **new** origin expressed in the **old** space.

Until Beta 1.2.1 this mod dropped that event on the floor. Everything it renders is relative to a
baseline it captured once, so a runtime recenter changed the raw pose, the relative pose changed
with it, and the asylum rotated or slid around the player - the exact opposite of what the button
says. On Quest Link that fights the "Reset View" in the menu; on SteamVR alone, where that
dashboard recenter is the only recenter a player has, it reads as "recenter does nothing / head
tracking is broken".

The fix is exact rather than a re-zero, so the runtime can move the player's chair without moving
their world:

```
baseQ' = conj(q) * baseQ          baseP' = Rot(conj(q), baseP - t)
```

which makes `conj(baseQ') * (c_new - baseP') == conj(baseQ) * (c_old - baseP)` for every point -
i.e. every relative pose the camera and the layer poses are built from is **identical** before and
after. Applied when the compositor reaches the `changeTime` the runtime named, logged as
`[OLVR][REFCENTER]`. If a runtime sends the event without a pose, the mod falls back to its own
recenter (re-zero), which is the only sane answer without geometry. `tests/stream_edge_fill_test.py`
checks the invariance numerically, including the orientation transform (the part that is easy to
get wrong and looks fine until you turn).

## 7. The swapchain format the runtime prefers (and what the mod now asks for)

OpenXR's performance guidance is to use the first colour format the runtime lists from
`xrEnumerateSwapchainFormats`, because anything else makes the compositor run a conversion pass
over every eye every frame - frame time, and therefore latency, spent outside the game's control.
Until Beta 1.2.1 this mod asked for `B8G8R8A8_UNORM_SRGB` unconditionally and never looked.

**It now negotiates, within a box narrow enough to be safe:**

- `BGRA8` sRGB is still requested first, and on every runtime this mod has run on (VDXR, SteamVR,
  Oculus/Meta) it is offered - so nothing changes there, not one pixel.
- If a runtime does **not** offer it but does offer plain `BGRA8` UNORM, that one is requested
  instead. It is the *same byte layout*, so every `CopyResource`/`CopySubresourceRegion` in the
  pipeline stays format-legal, and the only thing that moves is where the sRGB transfer happens:
  an sRGB render target encodes on write, a UNORM one does not, so the blit's gamma LUT is built a
  second time carrying **pre-encoded** values (`SrgbFromLinear`) and bound for exactly those draws.
- If the runtime offers neither, the shipped request stands and the runtime substitutes, exactly as
  every earlier build did.
- Nothing else is ever selected: `R8G8B8A8` would swap red and blue in the non-shader fallback copy
  and 10-bit formats would need a different readback, so both are ignored.

The escape hatch back to the old behaviour is `[XR] RuntimeFormat=0`. The log says what happened:

```
[OLVR][SWAPFMT] the runtime offers 8 swapchain formats, first: 91,87,29,28,... | BGRA8_SRGB offered, BGRA8 offered
[OLVR][SWAPFMT] eye images will be B8G8R8A8_UNORM, not sRGB: the blit writes sRGB-encoded bytes itself ...
[OLVR][GAMMAFMT] eye swapchain: asked fmt=87 (B8G8R8A8_UNORM), runtime allocated fmt=87 (exact match)
```

The sRGB transfer pair is checked numerically in `tests/stream_edge_fill_test.py` (round trip to
1e-4, endpoints clamped), because a wrong constant there is a picture that is one gamma step off -
the exact failure this whole section exists to avoid.

## 8. Reporting it

`OutlastVR_Log.txt` already answers the questions that matter, so attach it:

- `[XRRUNTIME]` / `[FOVMUTABLE]` / `[FITFRUSTUM]`: which runtime, and which path was taken.
- `[OLVR][REFCENTER]`: whether you used the headset's own recenter, and whether the mod could
  compensate for it.
- `[OLVR][SWAPFMT]`: the formats the runtime offers (per-runtime performance/gamma differences).
- `[OLVR][STREAM]`: the game's frame rate against the headset's refresh.
- `[OLVR][XRTIMING]`: the compositor loop's cost and the real display step (13.9 ms = 72 Hz,
  11.1 = 90 Hz; steps of 22.2 ms mixed in mean every second frame is synthesised).
- `[OLVR][XRSUBMIT]`: the FOV and the rect actually declared per eye.
