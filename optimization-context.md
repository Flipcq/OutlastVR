# Outlast VR — Optimization Context

Diagnosis of where frame time goes, what has already been optimized, remaining opportunities, and the measurement plan. Everything anchored to file:line in the actual code.

## 1. How a frame works today (and where the costs are)

```
game thread ── Hook_CalcSceneView (ol_ue3.cpp:7227) ── 2× o_CalcSceneView (L/R eye)
                                                    └─ OLXR_FrameCommit + OLInteract_Tick (ol_interact.cpp:3123)
render thread ── Hook_GetViewPoint (ol_ue3.cpp:6751) per eye  (pose, lean, IPD)
               ── draws (2× the whole game) ── per-draw hooks (d3d9_proxy.cpp:870-886)
               ── Hook_Present (d3d9_proxy.cpp:901)
                    ├─ PaceToHeadset()      ← waits for the worker to consume (ol_xr.cpp:5085)
                    ├─ PublishBackbuffer()  ← StretchRect bb → shared D3D9Ex frame (ol_xr.cpp:2844)
                    ├─ monitor mirror       (d3d9_proxy.cpp:308, 30 fps)
                    └─ o_Present            (window, IMMEDIATE)
worker XR ── xrWaitFrame → beginFrame → SyncInput → pose
          ── ConsumeUpload → ExWorkerTake: samples the shared frame in place (no copy; [PERF-A3])
          ── BlitHalfToEye ×2 → GammaDraw (shader + cached SRV + CopyResource) (ol_xr.cpp:4166/3873)
          ── xrEndFrame (up to 4 layers: projection + flip screen + HUD + focus)
```

The repo already measured the bottleneck (docs/dev/PROJECT-BASELINE.md:348): post-processing effects change nothing — the limit is the render thread submitting 2× the draw calls. Strategy: cut per-eye render CPU and redundant work in the delivery path, without touching image quality.

## 2. Already optimized (don't repeat)

| Already done | Where | Measured gain |
|---|---|---|
| D3D9Ex + shared frames (no readback) | d3d9_proxy.cpp:1285 | 46 → 74-81 fps |
| [TEXSHADOW] texture shadow in RAM | d3d9_proxy.cpp:1329 | 136 freezes of 0.6 s → 0 |
| [MIRRORFPS] monitor at 30 fps | d3d9_proxy.cpp:953 | +1.6 ms/frame |
| [OCCLSKIP] occlusion boxes not drawn, immediate answer | d3d9_proxy.cpp:1246 | +7-12% |
| [HELDFRAME] don't re-blit repeated frame | ol_xr.cpp:~5711 | ~40% of frames |
| Hook SetVertexShaderConstantF removed (14k calls/frame) | d3d9_proxy.cpp:1525 | gives back cycles |
| [LENSFIT] per-eye frustum | ol_ue3.cpp:7092 | same cost, more sharpness |
| [NATIVEEYE] replay off (kNativeEyeReplayEnabled=false) | d3d9_proxy.cpp:273 | — |

## 3. Opportunities by (impact ÷ risk)

### A. Cheap, safe, near-immediate gain

| # | What | Where | Why it gives gain | Risk |
|---|---|---|---|---|
| A1 | timeBeginPeriod(1) at startup, + THREAD_PRIORITY_TIME_CRITICAL and EcoQoS opt-out (SetThreadInformation(ThreadPowerThrottling)) on the XR worker thread, + optional process priority | OLXR_Init (ol_xr.cpp:2053) and LogWriter; no timeBeginPeriod/SetThreadPriority exists in the project today (grep empty) | PaceToHeadset uses Sleep(1): since Win10 2004 timer resolution is per-process, and without it Sleep(1) can sleep up to ~15.6 ms → blowing the 1.25×period budget and causing jitter. EcoQoS can demote the compositor thread on power-managed machines | very low |
| A2 | Replace YieldProcessor() spin + PaceToHeadset lock with event wait (WaitOnAddress on g_pubSeq) | ol_xr.cpp:5089-5097 | in the last 1.5 ms before the deadline, the game render thread burns CPU spinning (~100% of one core) competing with the draws | very low |
| A3 | Eliminate the 36 MB per-frame CopyResource in ExWorkerTake (create one SRV per slot and bind directly into the blit) + cache the SRV GammaDraw creates every call | ol_xr.cpp:2902 and 3503 (CreateShaderResourceView per eye per frame, with comment "don't cache by pointer" — the safe solution is cache by swapchain image index, cleared on rebuild, see DestroyEyeSwapchains) | VRAM bandwidth: 36 MB/frame ≈ 2.6 GB/s | medium — **DONE in phase 4** ([PERF-A3], `[Perf] SharedFrameDirect=1`) |
| A4 | Per-frame snapshot of flags read in each draw call: OcclDrawSkipped() → OLUE3_OcclBypassOn() (cross-TU call), MirrorUiDraw → OLUE3_StereoMode/SplitActive | d3d9_proxy.cpp:866-886, 766 | 2k–6k draws × 2 eyes = up to ~12k calls/frame, each with 6–8 volatile reads and 2–3 non-inlinable calls | low |
| A5 | Hook_SetRenderTarget does GetBackBuffer (AddRef/Release) on every RT switch | d3d9_proxy.cpp:443-452 | cache the backbuffer pointer (invalidated on Reset) | very low |
| A6 | Camcorder HUD: only resend the dynamic strip when text changes | ol_xr.cpp ~3975 (UpdateSubresource 1920×160×4 = 1.2 MB/frame) | upload + glyph rebuild every frame | very low |
| A7 | OLUE3_ObjScanStep: adaptive budget with QPC (e.g. stop at 0.3 ms) instead of fixed 4000 objects/frame | ol_ue3.cpp:2111, 2156 | SEH-guarded scan of 4000 objects every frame on the game thread | low |
| A8 | Take the monitor mirror off the render thread (draw on the D3D11 bridge, which already has the frame) | d3d9_proxy.cpp:308, 974 | 2 full-frame StretchRects on the render thread, and stops dirtying the shared backbuffer | low |

### B. Medium gains — need A/B testing on the null headset

| # | What | Detail / how to validate |
|---|---|---|
| B1 | Per-eye occlusion instead of "everything visible" | Today OcclForce=1 + OcclSkip=1 trades real culling for "always visible". Fix properly = map query→(eye, index) in Hook_CreateQuery/Hook_QueryGetData, keeping previous-frame answer, without wait=TRUE. Expected result: the +7-12% of OcclSkip and culling back. A/B: `occlskip 0` |
| B2 | 2nd eye reuses 1st eye's PVS | Hook_PvsGet (ol_ue3.cpp:6947) already in place; cache per-frame the result from eye L and return it to R |
| B3 | Do eye blits before xrWaitFrame | Capture was already moved out of the deadline for this reason (ol_xr.cpp:5140); the blits (BlitHalfToEye) still run inside the compositor window (ol_xr.cpp:5770+). Opening the window tends to reduce dropped frames/ASW |
| B4 | Single swapchain with arraySize=2 | Official recommendation (MS OpenXR best practices: "avoid a separate swapchain per eye / double-wide — runtime post-processing"). Today there are 2 swapchains (ol_xr.cpp:3113-3130). Gain depends on the runtime: measure in VDXR, SteamVR and Link |
| B5 | Fewer composition layers | In-game with camcorder in hand: projection + flip screen + HUD + focus = 4 layers (ol_xr.cpp:6154-6196). Draw HUD/focus inside the eye image (the blit already exists) eliminates compositor work per layer — test visually for loss of depth adjustment |
| B6 | FSR1 (EASU + RCAS) on the D3D11 blit | The blit is already a PS 4.0 with gamma LUT and custom CAS (ol_xr.cpp:3233). Swapping bicubic+sharpen for EASU+RCAS (pure HLSL, MIT, GPL-3.0 compatible like ImGui/MinHook) allows RenderScale 0.70-0.85 with better quality → real GPU gain for those who are GPU-bound (Quest 3 at 1.0 = 9.1 MP/frame) |
| B7 | "Performance" profile in launcher | Expose via ConsoleCommand (the con channel already exists, PROJECT-BASELINE.md:350): DynamicShadows (+6% measured), MaxDrawDistanceScale, Decals, ParticleLODBias, with restore on exit and an ini behind it (house rule) |
| B8 | Render directly into the swapchain image (typeless RTV) | Eliminates g_blitTmp + CopyResource (ol_xr.cpp:3547) = another 36 MB/frame. There was an old garbled-image case; the most likely hypothesis is wrong sRGB view — test only as an isolated experiment, with automatic fallback. **DONE in phase 4** as the experiment it was meant to be: `[Perf] DirectSwapchain=1` (off by default), format + colour space taken from the image (OlrDirectViewFormat), scratch draw + copy on any refusal |

### C. Structural — biggest gain, requires reverse engineering (research spike)

| # | What | Why it's the biggest gain available |
|---|---|---|
| C1 | Reuse eye L visibility for eye R (ComputeViewVisibility): copy PrimitiveVisibilityMap + dynamic element lists | The two eyes are ~3 cm apart: the visible set is practically identical. Skipping the 2nd eye's culling cuts about half of per-frame visibility CPU. First step: instrument (QPC) ComputeViewVisibility cost per eye and print in [PERF] — without numbers, don't touch it |
| C2 | Intermediate step for C1: union frustum (L ∪ R) on both passes | Classic multi-pass technique (Unity "optimized multi-pass"): guarantees identical sets → makes C1 trivially valid; alone it doesn't save CPU |
| C3 | Dynamic shadows once per frame (share shadow maps between eyes) | DynamicShadows off = +6%: pure duplicated cost per eye. Confirm with stat scenerendering — UE3 already shares the shadow map between views |
| C4 | Pose latency | There's already 1-period prediction — don't repeat without new data (only makes sense with blits out of the deadline, B3) |

### D. Duplicate work in Unreal Engine 3

**D.0 — How to prove it, using the engine's own tools**

Epic documents that, with two views, render thread cost doubles, and the two metrics that matter are Processed Primitives and Mesh draw calls. The `stat splitscreen` stat gives processed primitives, mesh draw calls and particle draw calls; the mod already has the `con` channel (PROJECT-BASELINE.md:350).

| Command (via con) | What it answers |
|---|---|
| stat unit | ms of Game / Draw (render thread) / GPU — tells which side is the limit now |
| stat splitscreen | processed primitives × mesh draw calls × particle draw calls |
| stat initviews | time and effectiveness of visibility culling |
| stat scenerendering | how much of render thread is in RenderViewFamily |
| stat sceneupdate | repeated AddLight/AddPrimitive |
| TRACKPARTICLERENDERINGSTATS → DUMPPARTICLERENDERINGSTATS | which particle systems cost (two reports in logs folder) |

Suggested mod instrumentation (per-frame counters, [PERF] pattern): CalcSceneView, UpdateSkelPose, GetFovAngle, PvsGet, OcclUpdate, NativePlayerMove, GetViewPoint.

**D.1 — "Are components updated twice per frame?" — answer is almost always no**

| System | Times per frame | Evidence |
|---|---|---|
| World / component / particle Tick (UParticleSystemComponent::TickComponent) | 1× | Update is per component on the game thread (TickGroup=TG_DuringAsyncWork, bSkipUpdateDynamicDataDuringTick in class defaults — UE3 source) |
| Animation / UpdateSkelPose (mod hooks 0x6FFC90) | 1× per component | it's the game thread update (ol_ue3.cpp:2786-2791); CalcSceneView only builds the FSceneView |
| CalcSceneView | 2× (the mod calls it) + N× if SceneCapture present | ol_ue3.cpp:7307-7315 |
| Culling / visibility (ComputeViewVisibility, ProcessVisible(ViewIndex,…)) | 2× | FSceneRenderer::ProcessVisible(INT ViewIndex, DWORD& ViewVisibilityMap, …) — class docs |
| Base pass / draw calls (RenderDPGBasePassStaticData(DPGIndex, FViewInfo& View)) | 2× | idem — it's the cost measured in the repo |
| Shadow maps (CreateWholeSceneProjectedShadow, GatherShadowsForPrimitives(…, BYTE ValidViews, …)) | 1× per family | ValidViews + per-view dependency show shared shadow map |
| Dynamic elements (skeletal: bone palette upload; particles: instances/LOD) | 2× (per view) | what generates a draw call per view |
| Post-process, fog volumes, translucency, decals, light shafts, distortion | 2× (RenderView per view) | idem |

⚠️ Correction: the "+6%" measured with DynamicShadows off is probably the base shadow cost, not a second shadow map render per eye. Before investing in "shadows once per frame", confirm with stat scenerendering — UE3 already shares the shadow map between views.

**D.2 — What the mod code duplicates (and can cut now)**

| # | Finding in code | Why it costs | Action |
|---|---|---|---|
| D-1 | Hook_SkelUpdate → PoseApplyVisible(comp) runs for every skeletal component in the level | the solver gate + counter + three global restores run for every component | exit with a mesh pointer read; restores still happen 1× per frame |
| D-2 | Hook_GetViewPoint calculates lean vector per eye | the lean vector is constant for the frame | share the lean vector between both eyes (per-eye body kept: costs ~300 ns, not worth rewriting the hook) |
| D-3 | GetFovAngle called several times per frame | repeated getter | cache indexed on three entries (frame, engine value, zoom) |
| D-4 | Player lights identified on every SetRotation | repeated lookup | identify once (NvLightCache) |
| D-5 | Objects scanned fixed: 4000/frame | SEH-guarded scan on the game thread | adaptive budget with QPC (A7) |

**D.5 — Official two-view lever not yet used by the mod: DetailMode**

Epic: "The next best method is setting a component's DetailMode to High, which will cause it to not be rendered at all in splitscreen. (…) Removing components in this way is the most effective for performance, because it reduces 'Processed primitives' and 'Mesh draw calls'."

In game: `con scale set DetailMode 1` (Medium) → every mesh marked High detail (vegetation, papers, debris, decoration, some effects) stops being processed in both views. Testable today via the con channel, measurable with stat splitscreen, reversible, visual cost assessable in 2 minutes. Less aggressive alternative (documented in the same text): aggressive MaxDrawDistance / cull distance — reduces draw calls but not processed primitives.

If approved: `[Render] DetailMode` ini option + launcher toggle (house rule: new behavior behind ini, log tag [DETAILMODE], restore original value when disabled).

**D.6 — Two hypotheses already dead (don't repeat)**

- Outlast custom occlusion tracker (PrimitiveSceneProxyOcclusionTracker, 0x29AEF0): hook installed, calls = 0 in gameplay, force ON changed nothing (ol_ue3.cpp:6915-6923).
- PVS bypass (GetPrecomputedVisibilityData, 0x8A7810): forced for whole windows, flicker unchanged (ol_ue3.cpp:6937-6948). The hook remains useful only for [FRAMETAG].
- What fixed two-view flicker was forcing hardware occlusion query results to "visible" (OcclForce + OcclSkip) — exactly item B1.

**D.7 — Where category D fits in execution order**

1. D.0 — instrumentation + stat splitscreen/stat initviews (1 session, zero risk). Before anything.
2. D-1 to D-5 — cheap gates on hooks that run per-component/per-query (immediate gain in NPC scenes, zero image risk).
3. D.5 — DetailMode as performance option, together with B7.
4. D.4 — only after DUMPPARTICLERENDERINGSTATS; if particles don't appear on top, discard.
5. C1/D.3 — visibility reuse between eyes (the only big structural gain left), with D.0 instrumentation measuring the target.

## 4. Measurement plan (without it, everything else is a guess)

The repo already has [PERF] (frame/present/grab/copy/pace/upload), [XRTIMING], [HITCH], [EXSTAT] and the SteamVR null headset. Three numbers are still missing:

1. GPU vs CPU: present avg/max already indicates GPU-bound (blocking Present), but a D3D11_QUERY_TIMESTAMP_DISJOINT around the blits is missing → real GPU cost of the bridge (2 eyes + copies).
2. Per-stage render thread cost: timer inside Hook_CalcSceneView (2 views) vs. rest of frame.
3. Draw calls per frame (g_uiSeen/g_occlSkippedDraws give a hint; a dedicated counter in Hook_DrawIndexedPrimitive closes the account) — this is the metric that validates A4, B1, C1.

**Protocol:** null headset, PaceToHeadset=0 + MaxSmoothedFrameRate=300 (as in PROJECT-BASELINE.md:351), 3 fixed game points (door, corridor, theater screen), 3 runs, median. Then confirm on a real Quest 3 with [HITCH].

## 5. Suggested execution order

1. Instrumentation (§4) — 1 session, zero risk.
2. A1, A2, A4, A5 — hitches and per-draw CPU, without touching image. Validation: [PERF]/[HITCH] before/after.
3. A3, A6, A7, A8 — wasted bandwidth and CPU.
4. B1, B2, B3 — the ones that change how the game draws → A/B via ini (OcclPerEye=1, PvsShare=1, BlitBeforeWait=1).
5. B6 (FSR1) and B7 (performance profile) — what the player feels as "more FPS".
6. C1/C2/C3 — only after the numbers, one at a time (house rule: "change one mechanic at a time").

## 6. Not worth it (already discarded by repo data)

- Post-processing quality (DOF, SSAO, bloom, light shafts): measured, changes nothing — the render thread is the limit.
- Single-pass stereo / multiview / SPS: requires instancing/vertex amplification and D3D11 in the renderer — impossible with UE3/D3D9 without rewriting the RHI. The viable path is C1 (visibility reuse), which attacks the same account from another side.
- Re-enabling AllocViewStateRva/SharedStateRva (runtime.ini:25-32): the file itself warns against it.
- Depth buffer submission (XR_KHR_composition_layer_depth) for positional reprojection: would require copying D3D9 depth every frame, and the README says to disable ASW/motion smoothing in this game — gain doesn't pay off.

## 7. External references used

- Khronos OpenXR Guide — Frame Submission: mandatory xrWaitFrame/xrBeginFrame pairing, late update/late latch, and why to submit the exact pose used for rendering (the mod already does — [FRAMETAG]).
- Microsoft — OpenXR best practices: pick the first swapchain format offered, use one projection, avoid dual layers/swapchain per eye, and the runtime post-processing warning (basis of B4/B5).
- MS — Surface sharing between Windows graphics APIs: D3D9Ex↔D3D11 sharing is unsynchronized (the mod already manages it with event queries) and format/mip/array restrictions (why A8R8G8B8, 1 mip, no MSAA in ol_xr.cpp:2802).
- VR/AR Wiki — Single-pass stereo rendering: two passes cost ~2× CPU/driver/geometry; and Unity's optimized multi-pass trick (shared shadow map + single cull against combined frustum) → exactly the design of C1/C2/C3.
- Bruce Dawson — Windows Timer Resolution: The Great Rule Change: since Win10 2004 timeBeginPeriod is per-process, so each app must request 1 ms resolution (A1).
- MS — SetThreadInformation / ThreadPowerThrottling: THREAD_POWER_THROTTLING_EXECUTION_SPEED = EcoQoS; must not be used on real-time/front threads — hence the explicit opt-out (A1).
- GPUOpen — FidelityFX FSR 1.0: pixel-shader upscaler (EASU + RCAS), MIT — fits in the existing D3D11 blit (B6).

## Roadmap of phases (implemented)

**Phase 1 — Instrumentation and diagnosis (no behavior change)**

- CPU/GPU and frametime measurement; draw call counters; visibility and render profiling (`stat splitscreen`, `stat initviews`, `stat scenerendering`).
- Draw hooks in d3d9_proxy.cpp (draw calls submitted + swallowed by [OCCLSKIP]).
- Hook_CalcSceneView in ol_ue3.cpp (µs inside the two CalcSceneViews).
- Per-frame counters for UpdateSkelPose, GetFovAngle, PVS and occlusion tracker.
- GPU of the mod's own commands (eye blits + copies) via D3D11_QUERY_TIMESTAMP_DISJOINT.
- docs/dev/PERFORMANCE.md with full protocol and Epic targets.

**Phase 2 — CPU optimization (each with a [Perf] switch, default = optimized path)**

- A4 DrawFastFlags: 3 cross-module calls per draw call → 1 snapshot per Present.
- A5 BackBufferCache: GetBackBuffer (COM, AddRef/Release) on every RT switch → pointer comparison; invalidated on Reset and new device.
- D1 SkelFastGate: solver gate exits with a mesh pointer read; restores still happen 1× per frame.
- D2: lean vector is constant per frame, shared between both eyes.
- D3 FovCache: getter cached (frame, engine value, zoom).
- D4 NvLightCache: player lights identified once.
- A6 HudSmart: dynamic strip (1.2 MB) only redrawn/sent when a readout changes.
- A7 ObjScanBudgetUs=250: object scan limited by time, not 4000 objects/frame.

**Phase 3 — Delivery path and XR thread**

- A1: timeBeginPeriod(1) per process; XR thread at THREAD_PRIORITY_TIME_CRITICAL + EcoQoS opt-out; ABOVE_NORMAL process available but disabled by default.
- A2: WaitOnAddress instead of spin (PaceToHeadset / ConsumeUpload); render thread blocks on g_conSeq instead of spinning with YieldProcessor().
- A8: monitor mirror off the render thread (worker draws directly into a DXGI bitblt swapchain on the same HWND, DXGI_PRESENT_DO_NOT_WAIT; live fallback if monitor not active).
- New [Perf] switches: TimerResolution=1, ThreadPriority=1, ProcessPriority=0, PaceWait=1, BridgeMirror=1 (all with optimized path as default), documented in runtime/outlastvr.ini.

**Phase 4 — Memory and GPU**

- A3, first half: the 36 MB per-frame copy is gone. `[EXSHARE]` used to `CopyResource` the opened shared frame into `g_uploadTex` before every blit; the blits now sample the shared texture itself. Every consumer of "this frame's pixels" reads one pointer, `FrameTex()` (null = the old upload texture, i.e. the CPU-readback lane, `[ZEROCOPY]`, and the fallback).
- A3, second half: the view cache. `CachedSrv`/`CachedRtv` keep one `ID3D11View` per (texture, kind, format) instead of `GammaDraw` creating and releasing one per eye per frame. The old comment refused to cache "by texture pointer" (a rebuilt swapchain frees the texture and the allocator reuses the address); the entry now holds a *reference* to its texture, so the identity cannot be recycled, and `ViewCacheClear()` runs in `DestroyEyeSwapchains` (before `destroySwapchain`), `RebuildForSize` and on every new `[EXSHARE]` generation. The MonitorDraw render-target view is deliberately NOT cached: a cached reference to a DXGI back buffer blocks `ResizeBuffers`.
- A3, ordering: the shared slot is only handed back when the frame's draws are queued, not when the copy runs — `ExDirectRelease()` ends the slot's D3D11 event query one frame later (`ExWorkerTake` → next frame), and `ExWorkerTick` skips the slot being read (its query still reads as signalled from an earlier frame — that check is the one that keeps the game from overwriting a live read). `RebuildForSize` releases it immediately, because a size change means the picture is unusable anyway.
- B8: `[Perf] DirectSwapchain` (default **0**, opt-in) draws the eye blit straight into the acquired swapchain image and skips `g_blitTmp` + `CopyResource`. `OlrDirectViewFormat` takes the view format *and* the colour space from the destination image: a typeless image gets an explicit format (null-desc views are illegal on typeless resources), an sRGB/UNORM mismatch is impossible by construction, anything outside the B8G8R8A8 family or not a render target is refused, and the frame falls back to the scratch draw + copy. That is the diagnosis of the old garbled-image case, expressed as code.
- New [Perf] switches: SharedFrameDirect=1 (optimized path, with its own automatic fallback), DirectSwapchain=0 (isolated experiment), documented in runtime/outlastvr.ini with a boot line each (`[PERF-A3]` / `[PERF-B8]`).
- Tests: `tests/perf_framepath_test.py` compiles the real cache and the real `OlrDirectViewFormat` against a stub D3D11 layer (view identity, reuse over 60 frames, refcount, refusal of a wrong-space view, eviction, clear) and then checks the source for the ordering rules above.

**Pending caveats:** missing visual test on a headset/real game session (the probe proves the mechanism, not the image); while A8 is active, present avg in the [PERF] report is 0 — that's the goal, not a regression. Phase 4 needs the same visual pass: `[PERF-A3] the shared frame is read in place` in the log says the copy is really gone, and `DirectSwapchain=1` must be judged on a headset (colour, seam, convergence shift) before it could ever become a default.

