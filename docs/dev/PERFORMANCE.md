# Performance work - measuring before and after

*Beta 1.2.2 optimization pass. For the switches themselves see `[Perf]` in
[runtime/outlastvr.ini](../../runtime/outlastvr.ini); for how the mod works inside see
[TECHNICAL.md](../TECHNICAL.md).*

The mod renders **two views per frame** in one `ViewFamily`, which is exactly what UE3 does for split
screen - and Epic's own guidance is blunt about what that costs:

> "The two big rendering thread costs are determining visibility and then submitting draw calls to the
> GPU. (…) the cost is doubled in split screen!"
> — [Render Thread Profiling and Optimization](https://docs.unrealengine.com/udk/Three/RenderThreadProfilingHome.html)

> "Splitscreen hits the rendering thread hard since twice as many things have to be drawn. There are
> two main costs on the rendering thread: the number of primitive components that have to be
> considered for rendering or *Processed Primitives*, and the total number of draw calls used to
> render visible meshes which is *Mesh draw calls*."
> — [Optimizing Content for Splitscreen](https://docs.unrealengine.com/udk/Three/OptimizingForSplitscreen.html)

So every change in this pass is judged against **draw calls**, **processed primitives** and the
**render thread time**, never against how it looks. Nothing here is a guess: each item below either
has a number next to it from the mod's own log, or is waiting for one.

---

## 1. The mod's own numbers (`[PERF]`, every 5 s in the log)

`%LOCALAPPDATA%\OutlastVR\OutlastVR_Log.txt` carries a line per 5 s. The fields that matter:

| Field | Meaning | What "good" looks like |
|---|---|---|
| `game X fps (frame avg a max b, >25ms N, >50ms M)` | Present-to-Present interval, and how many frames were long | `avg` close to `b`, `>50ms` at 0 |
| `present avg` | time blocked inside the game's own `Present` | a *large* value here means GPU-bound |
| `grab` / `copy` / `pace` | frame hand-over and pacing cost on the game thread | `pace` ~0 outside stereo transitions |
| `draws N/frame` | **draw calls the game submitted in the period** (one pass per eye) | compare before/after a change; it should not grow |
| `split N us avg M max` | time inside the mod's own two `CalcSceneView` calls (game thread) | a few hundred µs; a large `max` is a hitch |
| `skel N/frame (M solved)` | `UpdateSkelPose` calls vs the ones that reached the arm solver | `solved` ~1 per frame; `N` is the level's skeletal component count |
| `fov N/frame (M cached)` | how many times per frame the engine asks for the camera angle, and how many of those were answered from the per-frame cache | `cached` close to `N` once a frame is under way |
| `pvs N/frame` | `GetPrecomputedVisibilityData` lookups (InitViews: one per primitive per eye) | scales with primitives x 2 eyes |
| `gpu(own cmds) X ms` | GPU time of the mod's **own** command stream (eye blits + copies) | below ~1 ms; if it grows, the bridge is the problem, not the game |
| `occl per-eye: …` | how each occlusion answer was made: `fresh` from the GPU, `stale` from that eye's previous frame (the read did not wait), `first-read` (nothing to fall back on), `reused` (the 2nd eye took the 1st eye's verdict, C1), plus **one pass time per eye** and the PVS sharing counts | `first-read` near 0 after the first seconds; `stale` nonzero while the GPU is a frame behind (normal); pass L and R within a few tens of µs of each other |
| `[XRTIMING] loop …` | the compositor loop's cost and the real display step (13.9 = 72 Hz, 11.1 = 90 Hz) | 22.2 steps mixed in mean every second frame is synthesised |

Phase 3 adds one boot line each: `[PERF-A1]` says whether the 1 ms timer, the worker boost and
the optional process boost are on; `[PERF-A2]` says which pacing wait is in use; and `[PERF-A8]`
logs the moment the bridge takes the monitor over (`bridge owns the monitor: WxH`), or the reason
it handed it back. Phase 4 adds `[PERF-A3]` (is the shared frame sampled in place, or is the copy
still there) and `[PERF-B8]` (is the direct draw on), plus a one-off line the first time each lane
really runs. Phase 5 adds `[OCCLPEREYE]` (is each eye keeping its own occlusion answer - the default
- or is the old force-visible behaviour back), `[PVSSHARE]` (does the second eye reuse the first
eye's precomputed-visibility answer), `[VISREUSE]` (armed or idle, and why) and `[PERF-UNIONFRUSTUM]`
(one frustum for both eyes or one each).

`[HITCH]` lists frames over 40 ms with the second they happened, next to whatever else the log says
happened then.

---

## 2. The engine's own numbers (via the DevBridge `con` command)

With `[Debug] DevBridge=1`, write `con <command>` to `%LOCALAPPDATA%\OutlastVR\dev\cmd.txt`.

| Command | What it answers |
|---|---|
| `con stat unit` | game / draw (render thread) / GPU ms - which side is the limit **now** |
| `con stat splitscreen` | *processed primitives* x *mesh draw calls* x *particle draw calls*: how much of the cost is per-view work |
| `con stat initviews` | how long **visibility culling** took and how much it rejected (the single biggest per-view cost) |
| `con stat scenerendering` | how much of the render thread is inside `RenderViewFamily` |
| `con stat sceneupdate` | `AddLight` / primitives re-added - Epic's own example is `AddLight` called 10x a frame |
| `con TRACKPARTICLERENDERINGSTATS`, play, then `con DUMPPARTICLERENDERINGSTATS` | which particle systems cost (two reports in the game's log directory) |

Epic's splitscreen targets, for scale on a 360-class budget: 1600 mesh draw calls, 1700 processed
primitives, 150 particle draw calls per frame at 33 ms. Particle draw calls are **~3x more expensive
than a static mesh draw call**, and skeletal/interp actors ~2x - so 150 particle draws is not "small".

A splitscreen lever the mod does **not** use yet: a primitive whose `DetailMode` is *High* is
**not rendered at all in split screen**, which cuts both processed primitives and draw calls. Test it
with `con scale set DetailMode 1` in a heavy area while watching `stat splitscreen` - it is a
content-level trade (detail props disappear), so it would ship as an option, never as a default.

---

## 3. The protocol

1. **SteamVR's null headset** (no headset needed): `[Debug] XrRuntimeJson` pointing at
   `steamxr_win64.json`, a long screen-off timeout, `pauseCompositorOnStandby=false`.
2. `[XR] PaceToHeadset=0` and `MaxSmoothedFrameRate=300`, so the game free-runs and the frame rate is
   the metric. Everything else at the shipped defaults.
3. Three fixed spots: the asylum gate (open view), the admin corridor (occluded), and the theater
   screen (menu path). Walk them the same way every time.
4. Three runs each; take the median of `game fps` **and** of `draws/frame`.
5. Change **one** `[Perf]` switch at a time (`SkelFastGate=0`, `DrawFastFlags=0`, …) and repeat. A
   switch that shows no difference in the log should not stay in the shipped ini as a claim.

## 4. What this pass changed

| Change | Where | Effect |
|---|---|---|
| draw-call counters | `d3d9_proxy.cpp` `[PERF]` | instrumentation only - produces `draws N/frame` |
| split timing | `ol_ue3.cpp` `Hook_CalcSceneView` | instrumentation only - produces `split N us` |
| GPU span of the mod's commands | `ol_xr.cpp` `[PERF-GPU]` | instrumentation only - produces `gpu(own cmds)` |
| `[PERF-A4]` flag snapshot | `d3d9_proxy.cpp` | 3 cross-module calls removed **per draw call** |
| `[PERF-A5]` backbuffer cache | `d3d9_proxy.cpp` | one COM call removed per render-target change |
| `[PERF-D1]` skeletal gate | `ol_ue3.cpp` | the solver's gate + 3 restores removed for every non-Miles skeletal component, every frame |
| `[PERF-D2]` lean once per frame | `ol_ue3.cpp` | one clamp/multiply pass removed per frame (small, exact) |
| `[PERF-D3]` FOV cache | `ol_ue3.cpp` | the FOV maths removed for repeat queries in the same frame |
| `[PERF-D4]` NV light identity | `ol_ue3.cpp` | the classification removed for lights already known |
| `[PERF-A6]` HUD smart redraw | `ol_xr.cpp` | 1.2 MB upload + a glyph pass removed on frames whose readouts did not move |
| `[PERF-A7]` scan time budget | `ol_ue3.cpp` | the object scan's cost per frame bounded by time instead of 4000 objects |

### Phase 3 - threading and synchronisation

| Change | Where | Effect |
|---|---|---|
| `[PERF-A1]` timer + scheduling | `ol_xr.cpp` `Init` / `WorkerMain` | `timeBeginPeriod(1)` for this process (Sleep(1) is 1 ms again), the XR worker at `THREAD_PRIORITY_TIME_CRITICAL` with the EcoQoS opt-out, optional `ABOVE_NORMAL` process priority |
| `[PERF-A2]` waking wait | `ol_xr.cpp` `PaceToHeadset` | the render thread blocks in `WaitOnAddress` on `g_conSeq` instead of spinning (`YieldProcessor`) through the last 1.5 ms of the budget |
| `[PERF-A8]` monitor on the bridge | `ol_xr.cpp` `MonitorPresent`, `d3d9_proxy.cpp` `Hook_Present` | while stereo is live the D3D11 worker draws the left eye straight into a DXGI monitor swap chain and presents with `DO_NOT_WAIT`; the render thread does neither the two full-frame `StretchRect` calls nor the windowed Present |

A8 keeps the old path as a live fallback: the proxy only stands down when the bridge has
presented recently - `OLXR_MonitorActive()` allows 400 ms, lengthened to two monitor intervals
when `[Render] DesktopMirrorFps` is very low - so a stopped worker, a refused swap chain
(`CreateSwapChain` on the game's HWND), a non-windowed mode, or a headset going to sleep all
return to `DrawOneEyeDesktopMirror` + `o_Present` on the next frame. The swap chain is
bitblt (`DXGI_SWAP_EFFECT_DISCARD`) on purpose: it can coexist with the game's D3D9 window, so
the fallback can hand the window straight back. While A8 is live, `present avg` in `[PERF]` is 0
because this thread is not presenting - that is the point, not a regression.

### Phase 4 - memory and GPU

| Change | Where | Effect |
|---|---|---|
| `[PERF-A3]` shared frame read in place | `ol_xr.cpp` `ExWorkerTake` / `FrameTex` - `[Perf] SharedFrameDirect=1`, **ON by default with automatic fallback** | the 36 MB `CopyResource` out of the shared frame, every frame at ~2.6 GB/s, is not issued at all; the eye blit samples the shared texture |
| `[PERF-A3]` view cache | `ol_xr.cpp` `CachedSrv` / `CachedRtv` / `ViewCacheClear` - `[Perf] ViewCache=1` | one `ID3D11View` per texture instead of a create + release per eye per frame (driver-side allocations); the entry holds its texture, so a rebuilt swapchain cannot leave a stale pointer. `0` = the old view-per-call |
| `[PERF-A3]` slot hand-back | `ol_xr.cpp` `ExDirectRelease`, `ExWorkerTick` | the shared slot is not returned to the game until the frame's draws are queued (the event query is ended a frame later), and the slot being read is skipped when the tick looks for free slots |
| `[PERF-B8]` direct draw | `ol_xr.cpp` `GammaDraw`, `OlrDirectViewFormat` - `[Perf] DirectSwapchain=1`, OFF by default | removes `g_blitTmp` + the second 36 MB copy; the view format and colour space come from the destination image, and any refusal falls back to the scratch draw + copy |

Which line in the log says it is live:

- `[PERF-A3] shared frame sampled in place (no per-frame copy)` at boot is the shipped state; the first
  frame that takes that lane adds `[PERF-A3] the shared frame is read in place - the per-frame copy is
  gone`. `[PERF-A3] a shared frame cannot be sampled on this adapter - keeping the copy` means the
  adapter refused a view on the shared resource and the old path is in use.
- `[PERF-B8] direct swapchain draw ON ...` at boot; a destination that refuses a render-target view
  logs `the destination gave no render-target view` once, and an image whose colour space differs
  from what the swapchain was asked for logs the exact pair - that is the gamma the copy path cannot
  see, and the reason the direct lane derives it from the image.
- `copy` in the periodic `[PERF]` line should drop; `gpu(own cmds)` should drop by the two copies'
  cost when B8 is enabled.

`[PERF-B8]` stays OFF by default: it is the experiment the optimization notes always said it should
be. Judge it on a headset (colour, SBS seam, convergence shift, the camcorder HUD on top), never on
the API results - the earlier direct-render attempt in this project scrambled the picture while every
D3D call returned success.


- **`Hook_GetViewPoint` (D-2, full dedupe).** The per-eye body is ~300 ns: two `cosf`/`sinf`, one
  `ViewLeanM` and a dozen multiplies, over frame-frozen inputs. Only the lean vector is shared
  (done); rewriting the injection to reuse the whole block across the two eye passes would put the
  head tracking at risk for well under a microsecond a frame. Measured cost, not a guess: `split N us`
  in the log covers the same kind of work.
- **Dynamic shadows once per frame.** UE3 already renders one shadow map set per *view family* and
  only registers per-view dependencies (`FSceneRenderer::GatherShadowsForPrimitives(..., ValidViews,
  ...)`, `CreateWholeSceneProjectedShadow`). The `DynamicShadows off` +6% in the project baseline is
  the shadows' own cost, not a duplicate - `stat scenerendering` confirms it before anything is built.
- **The Outlast occlusion tracker.** Hooked and counted (`occl N/frame`); it is **never called** in
  gameplay (0 calls), so there is nothing to cache there. Its counter is kept as evidence.

