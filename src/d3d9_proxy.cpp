// d3d9_proxy.cpp - Outlast (x64) d3d9.dll proxy: frame hook + OpenXR flat presentation.
// =============================================================================
// OLGame.exe (Binaries\Win64) statically imports exactly 4 names from d3d9.dll:
//   Direct3DCreate9, D3DPERF_BeginEvent, D3DPERF_EndEvent, D3DPERF_SetOptions
// so dropping this DLL next to the exe loads it first; it forwards everything to the
// real system d3d9.dll and patch two vtable slots:
//   IDirect3D9::CreateDevice   (slot 16) -> grab the device, install device hooks
//   IDirect3DDevice9::Present  (slot 17) -> per-frame XR submit
//   IDirect3DDevice9::Reset    (slot 16) -> log res changes
//
// Vtable-slot patching (not MinHook) is deliberate: it's what the Dishonored D3D9 probe
// uses and D3D9 has none of the flip-model/overlay vtable contention that forced the
// swapchain-wrapper approach on the DXGI games.
//
// Keys (polled on a dedicated thread; the game keeps all its input). This is the WHOLE list, and it
// is short on purpose: everything a player can press has to be something a stranger can press
// safely, so the fourteen developer chords that used to live here were removed for release.
//   INSERT = open and close the settings menu (arrows / WASD / mouse / d-pad inside it)
//   G      = recenter, rebindable in the menu, saved to [Input] RecenterKey
//   pad x2 = recenter from a gamepad (double tap, L1 by default), rebindable, saved to [Input] RecenterPad
//   VR controllers: click both thumbsticks together to recenter (ol_xr.cpp [XRINPUT])
// =============================================================================

#include <Windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>
#include <ShlObj.h>
#include <math.h>
#include <unordered_map>
#include "ol_xr.h"
#include "ol_mouse_use.h"
#include "ol_motion.h"
#include "ol_interact.h"
#include "ol_ue3.h"
#include "ol_menu.h"
#include "MinHook.h"

// ---------------------------------------------------------------------------
// logging  ->  %LOCALAPPDATA%\OutlastVR\OutlastVR_Log.txt
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_cs;
// (Test46) [ASYNCLOG] Every line used to go straight to the file with an fflush, from the game thread, the
// render thread and the XR worker alike, under one lock - a disk write in the middle of a frame, and a burst
// of them every ~10 s when the periodic status lines all fall due ("fps drops every 10 seconds"). Lines now
// go into a memory buffer (a copy under the lock, no I/O), stamped with the seconds since the log opened,
// and a writer thread puts them on disk every 200 ms.
static char*  g_logBuf = nullptr;
static size_t g_logLen = 0, g_logCap = 0;
static LARGE_INTEGER g_logT0 = {}, g_logFreq = {};
static DWORD WINAPI LogWriter(LPVOID) {
    char* mine = nullptr; size_t mineCap = 0;
    for (;;) {
        Sleep(200);
        size_t n = 0;
        EnterCriticalSection(&g_cs);
        if (g_logLen) {
            char* t = g_logBuf; size_t tc = g_logCap;
            g_logBuf = mine; g_logCap = mineCap;
            mine = t; mineCap = tc; n = g_logLen; g_logLen = 0;
        }
        LeaveCriticalSection(&g_cs);
        if (n && g_log) { fwrite(mine, 1, n, g_log); fflush(g_log); }
    }
}
extern "C" double OLProxy_LogTime() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return g_logFreq.QuadPart ? (double)(t.QuadPart - g_logT0.QuadPart) / (double)g_logFreq.QuadPart : 0.0;
}

static void OpenLog() {
    wchar_t* base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) || !base) return;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    swprintf_s(dir, L"%s\\OutlastVR", base);
    CreateDirectoryW(dir, nullptr);
    swprintf_s(path, L"%s\\OutlastVR_Log.txt", dir);
    CoTaskMemFree(base);
    // Share the handle for reading. _wfopen_s locks the file for the whole session, so the log
    // cannot be read WHILE the game runs - and this log is the only instrument for diagnosing a
    // live session. _wfsopen with _SH_DENYWR keeps exclusive write access but lets a reader in.
    g_log = _wfsopen(path, L"w", _SH_DENYWR);
    QueryPerformanceFrequency(&g_logFreq); QueryPerformanceCounter(&g_logT0);
    if (g_log) CreateThread(nullptr, 0, LogWriter, nullptr, 0, nullptr);
}
// [DEVBRIDGE] ol_devbridge.cpp - developer test harness, off unless [Debug] DevBridge=1.
extern "C" void OLDev_Init(const wchar_t* ini);
extern "C" void OLDev_OnPresent(IDirect3DDevice9* dev, long presentCount);
extern "C" int  OLDev_Pad(WORD* buttons, float* lx, float* ly, float* rx, float* ry);

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    char line[4096];
    int n = sprintf_s(line, "%9.3f ", OLProxy_LogTime());
    if (n < 0) n = 0;
    va_list ap; va_start(ap, fmt);
    int m = _vsnprintf_s(line + n, sizeof(line) - n - 1, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m < 0) m = (int)strlen(line + n);
    n += m; line[n++] = '\n';
    EnterCriticalSection(&g_cs);
    if (g_logLen + n > g_logCap) {
        size_t cap = g_logCap ? g_logCap * 2 : 65536;
        while (cap < g_logLen + n) cap *= 2;
        char* nb = (char*)realloc(g_logBuf, cap);
        if (nb) { g_logBuf = nb; g_logCap = cap; }
    }
    if (g_logLen + n <= g_logCap) { memcpy(g_logBuf + g_logLen, line, n); g_logLen += n; }
    LeaveCriticalSection(&g_cs);
}
extern "C" void OLProxyLog(const char* line) { Log("%s", line); }

// ---------------------------------------------------------------------------
// real d3d9.dll
// ---------------------------------------------------------------------------
static HMODULE g_realD3D9 = nullptr;
static void EnsureRealD3D9() {
    if (g_realD3D9) return;
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\d3d9.dll", sys);
    g_realD3D9 = LoadLibraryW(path);
}

// ---------------------------------------------------------------------------
// [SCREQ] render ABOVE the desktop resolution
// ---------------------------------------------------------------------------
// The VR image can never be sharper than the frame the game actually rendered, and the split
// halves that horizontally: at the observed 2560x1440 windowed backbuffer each eye gets only
// 1280x1440 before it is stretched across ~90 degrees of headset.
//
// Outlast boots 3840x2160 exclusive-fullscreen and then Resets to a WINDOWED backbuffer sized
// like the desktop of the monitor its window is on. ME1's mechanism (spoof GetMonitorInfoW,
// [[me1-render-above-display-res]]) does NOT port: the live log showed 11 spoofed rects and the
// game still asked 2560x1440. So the monitor-rect spoof stays (harmless, may help elsewhere)
// but the real lever is the D3D-side desktop query, GetAdapterDisplayMode - hooked below.
// Windowed D3D9 has no display-mode list to satisfy, so a backbuffer larger than the desktop is
// legal - the window simply overhangs the screen, which is irrelevant because the backbuffer
// is what gets captured, not the window.
//
// HARD RULE learned 2026-07-22 (the broken-resolution/no-VR build): the backbuffer must
// always be the size the ENGINE asked for. Forcing pp bigger at Reset makes the engine render
// its believed size into the corner of the bigger buffer, and the half-split then cuts through
// the middle of the right eye. Make the engine ask bigger; never resize behind its back.
//
// Armed ONLY when the ini asks for more than the real desktop; at or below native every hook is
// a pass-through, so a smaller monitor is never harmed. Only the PRIMARY rect is rewritten, so a
// second display stays honest.
typedef BOOL (WINAPI *GetMonitorInfoW_t)(HMONITOR, LPMONITORINFO);
typedef BOOL (WINAPI *GetMonitorInfoA_t)(HMONITOR, LPMONITORINFO);
static GetMonitorInfoW_t o_GetMonitorInfoW = nullptr;
static GetMonitorInfoA_t o_GetMonitorInfoA = nullptr;
static LONG g_reqW = 0, g_reqH = 0;         // what the ini asked for
static LONG g_spoofArmed = 0;
static volatile LONG g_dispqHits = 0;

// Spoof ANY monitor that is smaller than the request, not just the primary.
// Live log 2026-07-22 proved why: the primary IS 3840x2160, but the game resets to 2560x1440 -
// the size of the SECOND monitor, because that is where its window lives. A primary-only spoof
// rewrote a rect the game never asked about. Monitors already big enough are left honest, so
// this stays a pass-through wherever it isn't needed.
static void SpoofPrimary(LPMONITORINFO mi) {
    if (!g_spoofArmed || !mi) return;
    const LONG w = mi->rcMonitor.right - mi->rcMonitor.left;
    const LONG h = mi->rcMonitor.bottom - mi->rcMonitor.top;
    if (w >= g_reqW && h >= g_reqH) return;
    mi->rcMonitor.right  = mi->rcMonitor.left + g_reqW;
    mi->rcMonitor.bottom = mi->rcMonitor.top  + g_reqH;
    mi->rcWork = mi->rcMonitor;              // rcWork must follow or the engine clamps on that
    InterlockedIncrement(&g_dispqHits);
}
static BOOL WINAPI Hook_GetMonitorInfoW(HMONITOR h, LPMONITORINFO mi) {
    BOOL r = o_GetMonitorInfoW(h, mi);
    if (r) SpoofPrimary(mi);
    return r;
}
static BOOL WINAPI Hook_GetMonitorInfoA(HMONITOR h, LPMONITORINFO mi) {
    BOOL r = o_GetMonitorInfoA(h, mi);
    if (r) SpoofPrimary(mi);
    return r;
}

// The monitor-rect spoof alone was NOT it: 11 rects were rewritten and the game still asked
// 2560x1440 (live log 2026-07-22). The D3D-native way a UE3 D3D9 game reads "the desktop
// resolution" for windowed mode is IDirect3D9::GetAdapterDisplayMode - a vtable already owned.
// Raise any reported mode smaller than the request so the ENGINE itself asks for the big
// backbuffer; then its scene viewport, its UI, and the buffer all agree by construction.
typedef HRESULT (STDMETHODCALLTYPE *GetAdapterDisplayMode_t)(IDirect3D9*, UINT, D3DDISPLAYMODE*);
static GetAdapterDisplayMode_t o_GetAdapterDisplayMode = nullptr;
static volatile LONG g_admHits = 0;
static HRESULT STDMETHODCALLTYPE Hook_GetAdapterDisplayMode(IDirect3D9* This, UINT adapter, D3DDISPLAYMODE* m) {
    HRESULT hr = o_GetAdapterDisplayMode(This, adapter, m);
    if (SUCCEEDED(hr) && m && g_spoofArmed &&
        ((LONG)m->Width < g_reqW || (LONG)m->Height < g_reqH)) {
        LONG n = InterlockedIncrement(&g_admHits);
        if (n <= 3)
            Log("[OLVR][SCREQ] GetAdapterDisplayMode(adapter=%u) %ux%u -> spoofed %ldx%ld",
                adapter, m->Width, m->Height, g_reqW, g_reqH);
        m->Width = (UINT)g_reqW; m->Height = (UINT)g_reqH;
    }
    return hr;
}

// ---------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE *CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE *CreateQuery_t)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
typedef HRESULT (STDMETHODCALLTYPE *QueryGetData_t)(IDirect3DQuery9*, void*, DWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static CreateDevice_t o_CreateDevice = nullptr;
static Present_t      o_Present = nullptr;
static Reset_t        o_Reset = nullptr;
static LONG           g_d3d9HookDone = 0;
static LONG           g_devHookDone = 0;
static volatile LONG  g_presentCount = 0;
static volatile LONG  g_firstRunPending = 0;      // open the settings menu once, ever
static const LONG     kFirstRunOpenAtPresent = 400;
static volatile LONG  g_xrEnabled = 1;      // XR submit on by default; a crash-safe off switch
static volatile LONG  g_xrAllowed = 1;      // outlastvr.ini [XR] Enabled - set 0 to run flat
static volatile LONG  g_keepVsync = 0;      // [XR] KeepGameVsync=1 - leave the game's vsync alive (see [PACING])

// ---- SFR support: hold the device, and keep a copy of the left-eye frame ----------------
// SFR renders the whole frame twice per present, each pass a full-screen PRIMARY view. Pass 1
// (left eye) has to be saved off the backbuffer before pass 2 overwrites it.
static IDirect3DDevice9*  g_dev = nullptr;
static IDirect3DSurface9* g_leftRT = nullptr;
static UINT g_leftW = 0, g_leftH = 0;
extern "C" int OLUE3_SplitActive();
extern "C" int OLUE3_StereoMode();
// The game backbuffer is SBS by design. Keep that for the headset, but replace the desktop
// preview after XR capture with a full-window copy of the left eye.
static IDirect3DSurface9* g_desktopMirrorRT = nullptr;
static UINT g_desktopMirrorW = 0, g_desktopMirrorH = 0;
static volatile LONG g_desktopMirrorOneEye = 1;
static volatile LONG g_mirrorFps = 30;            // [MIRRORFPS] (Test46) monitor presents a second while in stereo; 0 = all
static volatile LONG g_mirrorSkipped = 0;
extern "C" void OLProxy_SetOcclSkip(int on);
extern "C" long OLProxy_OcclSkippedDraws();
extern "C" void OLProxy_SetMirrorFps(int f) { InterlockedExchange(&g_mirrorFps, f < 0 ? 0 : f); Log("[OLVR][MIRRORFPS] monitor at %d presents a second%s", f, f <= 0 ? " (every frame)" : ""); }
extern "C" long OLProxy_MirrorSkipped() { return InterlockedExchange(&g_mirrorSkipped, 0); }

// ---- native composite capture -----------------------------------------------------------
// UE3 renders each split view through a full-size scene/post-process texture, then composites
// that texture into one half of the desktop backbuffer. Reading the backbuffer therefore throws
// away half of the horizontal detail before OpenXR ever sees it. Catch the two final scene
// composites while their full-size source textures are still bound and preserve them in a
// private side-by-side surface. The desktop/backbuffer remains completely unchanged.
static IDirect3DSurface9* g_nativeStereoRT = nullptr;
static UINT g_nativeEyeW = 0, g_nativeEyeH = 0;
static D3DFORMAT g_nativeFmt = D3DFMT_UNKNOWN;
static volatile LONG g_nativeCompositeCount = 0;
static volatile LONG g_nativePairValid = 0;
static LONG g_nativePairs = 0, g_nativeMisses = 0;
// Test 28 performance route.  Replaying the final composite into a 3840x1080 target and then
// synchronously reading it back reduced the measured game rate to roughly 40 fps on the test
// machine.  The regular 1920x1080 backbuffer is already tone-mapped by Outlast and retains the
// corrected D3D11 upload path, so submit that directly and avoid the extra render/readback cost.
static constexpr bool kNativeEyeReplayEnabled = false;

extern "C" int OLUE3_BuildRightFirst();

extern "C" IDirect3DSurface9* OLProxy_AcquireNativeStereo() {
    IDirect3DSurface9* s = g_nativeStereoRT;
    if (!s || !g_nativePairValid) return nullptr;
    s->AddRef();
    return s;
}

extern "C" IDirect3DSurface9* OLProxy_LeftEye() { return g_leftRT; }

extern "C" int OLProxy_CaptureLeftEye() {
    if (!g_dev) return 0;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return 0;
    D3DSURFACE_DESC sd;
    if (FAILED(bb->GetDesc(&sd))) { bb->Release(); return 0; }
    if (!g_leftRT || g_leftW != sd.Width || g_leftH != sd.Height) {
        if (g_leftRT) { g_leftRT->Release(); g_leftRT = nullptr; }
        if (FAILED(g_dev->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0,
                                             FALSE, &g_leftRT, nullptr))) {
            g_leftRT = nullptr; bb->Release();
            Log("[OLVR][SFR] left-eye RT create FAILED %ux%u", sd.Width, sd.Height);
            return 0;
        }
        g_leftW = sd.Width; g_leftH = sd.Height;
        Log("[OLVR][SFR] left-eye RT %ux%u", sd.Width, sd.Height);
    }
    HRESULT hr = g_dev->StretchRect(bb, nullptr, g_leftRT, nullptr, D3DTEXF_NONE);
    bb->Release();
    return SUCCEEDED(hr) ? 1 : 0;
}

static void DrawOneEyeDesktopMirror() {
    // SBS leaves two half-width eyes in the backbuffer, so the desktop needs the left half
    // enlarged. SFR leaves one complete eye in the backbuffer after its second full-screen pass;
    // cutting that image in half here was the source of the misleading desktop/headset mismatch.
    if (!g_desktopMirrorOneEye || !g_dev || !OLUE3_SplitActive() || OLUE3_StereoMode() != 1) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC sd = {};
    if (FAILED(bb->GetDesc(&sd)) || sd.Width < 2 || !sd.Height) { bb->Release(); return; }
    if (!g_desktopMirrorRT || g_desktopMirrorW != sd.Width || g_desktopMirrorH != sd.Height) {
        if (g_desktopMirrorRT) { g_desktopMirrorRT->Release(); g_desktopMirrorRT = nullptr; }
        if (FAILED(g_dev->CreateRenderTarget(sd.Width, sd.Height, sd.Format,
                                             D3DMULTISAMPLE_NONE, 0, FALSE,
                                             &g_desktopMirrorRT, nullptr))) {
            bb->Release(); return;
        }
        g_desktopMirrorW = sd.Width; g_desktopMirrorH = sd.Height;
        Log("[OLVR][MIRROR] one-eye desktop preview ready %ux%u", sd.Width, sd.Height);
    }
    RECT left = { 0, 0, (LONG)(sd.Width / 2), (LONG)sd.Height };
    if (SUCCEEDED(g_dev->StretchRect(bb, &left, g_desktopMirrorRT, nullptr, D3DTEXF_LINEAR)))
        g_dev->StretchRect(g_desktopMirrorRT, nullptr, bb, nullptr, D3DTEXF_LINEAR);
    bb->Release();
}

// ---------------------------------------------------------------------------
// [UIMIRROR] draw the UI into BOTH eyes instead of chopping it in half
// ---------------------------------------------------------------------------
// The split renders the WORLD twice, into the left and right halves of the backbuffer. The UI
// is not part of that: UE3 draws the HUD and the menus ONCE, across the full frame, after both
// views are done. The frame is then cut down the middle, so the left half of the UI goes to the
// left eye and the right half to the right eye - each eye sees a DIFFERENT piece of the menu,
// centred. The brain fuses them, so left-of-centre content appears where right-of-centre content
// should be. That is exactly the "small and cross eyed, left is on the right" symptom, and it is
// the same class of bug ME2 hit ("HUD was RIGHT-EYE ONLY") - see [[me2-vr-ui-catchers]].
//
// Fix: catch UI draws and issue each one TWICE, once into a box inside the left half and once
// into the matching box in the right half, instead of once across the whole frame. Both eyes
// then get the COMPLETE UI at the same place, so it fuses at screen depth.
//
// The box is aspect-corrected rather than just "the half". An eye's image is (bbW/2 x bbH)
// pixels stretched across the rendered FOV, so squeezing a 16:9 UI into the raw half would show
// it stretched - ME2's [UIRATIO] bug. Solving for equal angular aspect gives
//     boxH = bbH * tan(fovH/2) / (tan(fovV/2) * uiAspect)
// which at 90x96 deg and a 16:9 source is ~51% of the height, centred.
//
// Classifier (ME2's, which they validated): a UI draw is alpha-blended with dest=INV_SRC_ALPHA
// and depth testing OFF. Full-screen post/darken/multiply quads fail that test and are left
// alone, so they cannot leak into one eye. On top of that the draw must span the FULL
// backbuffer width: during a split the engine renders each eye through a HALF-width viewport, so
// anything still full-width is by construction not part of either eye's scene. That second gate
// comes free from the split itself and is the strong one.
static UINT g_bbW = 0, g_bbH = 0;
// The private native-eye target is wider than Outlast's real backbuffer.  OpenXR must use the
// game's logical eye aspect (half of this backbuffer), not the private target's storage aspect,
// when asking UE3 to widen its camera.  Otherwise the 3840-wide pair makes UE3 render roughly
// 130x135 degrees and throws away a large part of the available pixel density.
extern "C" void OLProxy_GetGameBackbuffer(UINT* w, UINT* h) {
    if (w) *w = g_bbW;
    if (h) *h = g_bbH;
}
// MODES: 0 = off, 1 = ON (default), 2 = MENUS-ONLY. Set in the ini, [Render] UiMirror.
//
// History, because this has now failed in two opposite directions in one day:
//  * Mirroring every candidate WASHED THE COLOURS OUT. Cause found: the classifier treated an
//    untextured draw as UI, so Outlast's own full-screen darkening/fade passes were relocated
//    into the half-height box and the frame stopped being darkened. Fixed properly in
//    ClassifySourceTex - a HUD element is TEXTURED, a darkening pass is not.
//  * Volume-gating it to menus (27+ catches/frame) then LOST THE CAMCORDER RECORD ICON, because
//    the in-game HUD is exactly the low-volume case that gate was designed to exclude. Volume
//    was never the right question; it was a proxy for a classifier that was broken.
// So: mirror on the corrected classifier, all the time. Mode 2 keeps the volume gate as a
// fallback in case some other overlay slips through - it is a diagnostic, not the plan.
#define UIMIRROR_OFF 0
#define UIMIRROR_ON 1
#define UIMIRROR_MENUS 2
static volatile LONG g_uiMirror = UIMIRROR_ON;
static volatile LONG g_uiMirrored = 0, g_uiSeen = 0;
static volatile LONG g_uiFrameHits = 0, g_uiFrameHitsLast = 0;
static volatile LONG g_uiRejNoTex = 0, g_uiRejFull = 0;   // why candidates were passed through
static volatile LONG g_uiAutoArmed = 0;      // mode 2 only: is a menu currently open?
static LONG g_uiQuietFrames = 0;
static const LONG kUiMenuOn   = 15;
static const LONG kUiMenuOff  = 5;
static const LONG kUiQuietMax = 30;

// Called once per Present with the count of UI candidates seen in the frame just finished.
static void UiMirrorAutoTick(LONG hits) {
    if (g_uiMirror != UIMIRROR_MENUS) { g_uiAutoArmed = 0; g_uiQuietFrames = 0; return; }
    if (hits >= kUiMenuOn) {
        g_uiQuietFrames = 0;
        if (!InterlockedExchange(&g_uiAutoArmed, 1))
            Log("[OLVR][UIMIRROR] menus-only: armed (%ld UI draws in one frame)", hits);
    } else if (g_uiAutoArmed && hits < kUiMenuOff) {
        if (++g_uiQuietFrames >= kUiQuietMax) {
            InterlockedExchange(&g_uiAutoArmed, 0);
            g_uiQuietFrames = 0;
            Log("[OLVR][UIMIRROR] menus-only: disarmed (gameplay volume)");
        }
    } else if (g_uiAutoArmed) {
        g_uiQuietFrames = 0;
    }
}
static bool UiMirrorActiveNow() {
    const LONG m = g_uiMirror;
    return m == UIMIRROR_ON || (m == UIMIRROR_MENUS && g_uiAutoArmed);
}

typedef HRESULT (STDMETHODCALLTYPE *SetRenderState_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIndexedPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);

// FALSIFIED 2026-07-22, live: gating on "the viewport spans the full backbuffer" does NOT
// identify UI in this engine. Outlast renders each eye's scene FULL-SIZE into an offscreen
// target and only places it into a half at composite time, so every post-process pass is
// full-width too. Mirroring them halved the frame once per pass and compounded: 2 copies, then
// 4, then 8 tiny scenes across the screen.
//
// The gate that actually separates them is the RENDER TARGET. The scene and its whole post
// chain run on offscreen targets; only the final composite and the UI are drawn onto the BACK
// BUFFER. Composite is opaque, UI is alpha-blended - so "render target is the backbuffer" AND
// "alpha blended" AND "depth off" isolates the UI without relying on viewport geometry at all.
// If Outlast turns out to draw its UI offscreen too, this gate simply never fires: the mirror
// no-ops and nothing else is harmed, which is the correct way for this to fail.
typedef HRESULT (STDMETHODCALLTYPE *SetRenderTarget_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
static SetRenderTarget_t o_SetRenderTarget = nullptr;
static volatile LONG g_rtIsBackbuffer = 1;   // frame starts on the backbuffer

// Also the only honest way to tell whether UE3's ScreenPercentage supersampling actually took:
// if it did, the scene render target is LARGER than the backbuffer. Reported once per new
// biggest size, so the log answers "is the game rendering more pixels than it presents?".
static LONG g_maxRtW = 0, g_maxRtH = 0;

static HRESULT STDMETHODCALLTYPE Hook_SetRenderTarget(IDirect3DDevice9* This, DWORD idx, IDirect3DSurface9* surf) {
    HRESULT hr = o_SetRenderTarget(This, idx, surf);
    if (idx == 0) {
        LONG isBb = 0;
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(This->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            isBb = (bb == surf) ? 1 : 0;
            bb->Release();
        }
        InterlockedExchange(&g_rtIsBackbuffer, isBb);
        if (!isBb && surf && g_bbW > 0) {
            D3DSURFACE_DESC d;
            if (SUCCEEDED(surf->GetDesc(&d)) && (LONG)d.Width * (LONG)d.Height > g_maxRtW * g_maxRtH) {
                g_maxRtW = (LONG)d.Width; g_maxRtH = (LONG)d.Height;
                Log("[OLVR][SS] largest scene render target now %ux%u (backbuffer %ux%u) - "
                    "%s", d.Width, d.Height, g_bbW, g_bbH,
                    ((LONG)d.Width > (LONG)g_bbW) ? "SUPERSAMPLING is live"
                                                  : "at or below backbuffer size");
            }
        }
    }
    return hr;
}

static SetRenderState_t         o_SetRenderState = nullptr;
static DrawPrimitive_t          o_DrawPrimitive = nullptr;
static DrawIndexedPrimitive_t   o_DrawIndexedPrimitive = nullptr;
static DrawPrimitiveUP_t        o_DrawPrimitiveUP = nullptr;
static DrawIndexedPrimitiveUP_t o_DrawIndexedPrimitiveUP = nullptr;

// Mirror the three states classified here rather than calling GetRenderState per draw - there are
// thousands of draws a frame and each COM call would be pure overhead.
static volatile LONG g_rsZ = 1, g_rsBlend = 0, g_rsDest = 0;

static HRESULT STDMETHODCALLTYPE Hook_SetRenderState(IDirect3DDevice9* This, D3DRENDERSTATETYPE s, DWORD v) {
    switch (s) {
        case D3DRS_ZENABLE:          InterlockedExchange(&g_rsZ, (LONG)v); break;
        case D3DRS_ALPHABLENDENABLE: InterlockedExchange(&g_rsBlend, (LONG)v); break;
        case D3DRS_DESTBLEND:        InterlockedExchange(&g_rsDest, (LONG)v); break;
        default: break;
    }
    return o_SetRenderState(This, s, v);
}

extern "C" int OLUE3_SplitActive();
extern "C" void OLUE3_GetEyeFov(float* fx, float* fy);

// The final scene composite is ALSO a full-screen alpha-blended depth-off quad on the backbuffer,
// so the state test alone cannot tell it from UI. Mirroring it squeezed the whole already-SBS
// frame into each half and put TWO views in every eye (live, 2026-07-22).
//
// What separates them is the SOURCE TEXTURE: the composite samples a backbuffer-sized scene
// texture, while HUD and menu elements sample comparatively small atlases. Anything sampling a
// near-fullscreen texture is scene work, not UI.
// Three-way, because "not a fullscreen texture" was never the same thing as "is UI".
//
// 2026-07-22, the washed-out regression: the old two-way test returned false for a draw with NO
// texture bound at all, so every untextured full-screen fill - Outlast's fades, darkening and
// vignette passes - was classified as UI and got relocated into the half-height box. The frame
// then lost most of its darkening, which is what "washed out" was.
//
// A HUD element is a TEXTURED quad (icon, glyph atlas, battery bar). A screen darkening pass is
// either untextured or samples a scene-sized target. So: no texture = not UI, scene-sized
// texture = not UI, everything else = UI candidate.
enum UiTexVerdict { UITEX_NONE = 0, UITEX_SMALL = 1, UITEX_FULLSCREEN = 2 };
static UiTexVerdict ClassifySourceTex(IDirect3DDevice9* dev, UINT* outW, UINT* outH) {
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    IDirect3DBaseTexture9* bt = nullptr;
    if (FAILED(dev->GetTexture(0, &bt)) || !bt) return UITEX_NONE;
    UiTexVerdict v = UITEX_NONE;
    if (bt->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(((IDirect3DTexture9*)bt)->GetLevelDesc(0, &d))) {
            if (outW) *outW = d.Width;
            if (outH) *outH = d.Height;
            v = ((d.Width * 10 >= g_bbW * 6) && (d.Height * 10 >= g_bbH * 6)) ? UITEX_FULLSCREEN
                                                                             : UITEX_SMALL;
        }
    }
    bt->Release();
    return v;
}

static bool EnsureNativeStereoTarget(IDirect3DDevice9* dev, D3DFORMAT* outBackbufferFmt) {
    IDirect3DSurface9* bb = nullptr;
    D3DSURFACE_DESC back = {};
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    const HRESULT descHr = bb->GetDesc(&back);
    bb->Release();
    if (FAILED(descHr) || !back.Width || !back.Height) return false;
    if (outBackbufferFmt) *outBackbufferFmt = back.Format;

    if (g_nativeStereoRT && g_nativeEyeW == back.Width && g_nativeEyeH == back.Height &&
        g_nativeFmt == back.Format) return true;
    if (g_nativeStereoRT) { g_nativeStereoRT->Release(); g_nativeStereoRT = nullptr; }
    g_nativeEyeW = g_nativeEyeH = 0; g_nativeFmt = D3DFMT_UNKNOWN;

    D3DCAPS9 caps = {};
    if (FAILED(dev->GetDeviceCaps(&caps)) || back.Width * 2 > caps.MaxTextureWidth) {
        Log("[OLVR][NATIVEEYE] cannot allocate %ux%u SBS target (device max width %u)",
            back.Width * 2, back.Height, caps.MaxTextureWidth);
        return false;
    }
    if (FAILED(dev->CreateRenderTarget(back.Width * 2, back.Height, back.Format,
                                       D3DMULTISAMPLE_NONE, 0, FALSE,
                                       &g_nativeStereoRT, nullptr)) || !g_nativeStereoRT) {
        Log("[OLVR][NATIVEEYE] CreateRenderTarget FAILED for %ux%u fmt=%d",
            back.Width * 2, back.Height, (int)back.Format);
        g_nativeStereoRT = nullptr;
        return false;
    }
    g_nativeEyeW = back.Width; g_nativeEyeH = back.Height; g_nativeFmt = back.Format;
    dev->ColorFill(g_nativeStereoRT, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
    Log("[OLVR][NATIVEEYE] color-correct pair target ready: %ux%u (%ux%u per eye) outputFmt=%d",
        back.Width * 2, back.Height, back.Width, back.Height, (int)back.Format);
    return true;
}

// Called only for a scene-sized texture drawn onto the backbuffer. The source is HDR
// A16B16G16R16F (format 113), so copying its bytes directly to the OpenXR BGRA8 bridge corrupts
// the colours and skips Outlast's tone mapping. Replay the game's own final composite draw into
// a double-width BGRA8 target instead. A 3840-wide viewport expands the original left/right
// half rectangles to 1920 pixels each while the original shader performs the exact colour grade.
template <class F>
static void CaptureNativeSceneComposite(IDirect3DDevice9* dev, F&& draw) {
    if (!g_rtIsBackbuffer || OLUE3_StereoMode() != 1 || !OLUE3_SplitActive()) return;
    LONG ordinal = g_nativeCompositeCount;
    if (ordinal >= 2) return;

    IDirect3DBaseTexture9* base = nullptr;
    if (FAILED(dev->GetTexture(0, &base)) || !base || base->GetType() != D3DRTYPE_TEXTURE) {
        if (base) base->Release();
        return;
    }
    IDirect3DTexture9* tex = static_cast<IDirect3DTexture9*>(base);
    D3DSURFACE_DESC srcDesc = {};
    D3DFORMAT backFmt = D3DFMT_UNKNOWN;
    if (FAILED(tex->GetLevelDesc(0, &srcDesc)) ||
        srcDesc.Width * 10 < g_bbW * 6 || srcDesc.Height * 10 < g_bbH * 6 ||
        !EnsureNativeStereoTarget(dev, &backFmt)) {
        base->Release(); return;
    }

    IDirect3DSurface9* oldRt = nullptr;
    D3DVIEWPORT9 oldVp = {};
    if (FAILED(dev->GetRenderTarget(0, &oldRt)) || !oldRt || FAILED(dev->GetViewport(&oldVp))) {
        if (oldRt) oldRt->Release(); base->Release(); return;
    }
    DWORD scissorOn = FALSE;
    RECT oldScissor = { 0, 0, (LONG)g_bbW, (LONG)g_bbH };
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorOn);
    if (scissorOn) dev->GetScissorRect(&oldScissor);

    HRESULT replayHr = D3DERR_INVALIDCALL;
    if (SUCCEEDED(o_SetRenderTarget(dev, 0, g_nativeStereoRT))) {
        if (ordinal == 0) dev->ColorFill(g_nativeStereoRT, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
        D3DVIEWPORT9 wide = oldVp;
        wide.X = 0; wide.Y = 0; wide.Width = g_nativeEyeW * 2; wide.Height = g_nativeEyeH;
        dev->SetViewport(&wide);
        if (scissorOn) dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        replayHr = draw();
        if (scissorOn) {
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
            dev->SetScissorRect(&oldScissor);
        }
        o_SetRenderTarget(dev, 0, oldRt);
    }
    dev->SetViewport(&oldVp);
    oldRt->Release();

    if (SUCCEEDED(replayHr)) {
        ordinal = InterlockedIncrement(&g_nativeCompositeCount);
        if (ordinal == 2) {
            InterlockedExchange(&g_nativePairValid, 1);
            ++g_nativePairs;
            if (g_nativePairs == 1)
                Log("[OLVR][NATIVEEYE] first full-resolution pair replayed through Outlast tone mapping "
                    "(HDR sourceFmt=%d -> outputFmt=%d)", (int)srcDesc.Format, (int)backFmt);
        }
    }
    base->Release();
}

// UI and full-screen fades are authored after the two scene composites. Replay those draws into
// both halves of the private target so menus, HUD, subtitles, fades, and the settings overlay do
// not disappear when OpenXR switches from the low-resolution backbuffer to the native eye pair.
extern "C" void OLProxy_GetHudFit(float* scale, float* offX, float* offY);
template <class F>
static void ReplayNativeOverlay(IDirect3DDevice9* dev, F&& draw, bool fitUi) {
    if (!g_nativePairValid || !g_nativeStereoRT || !g_nativeEyeW || !g_nativeEyeH) return;
    IDirect3DSurface9* oldRt = nullptr;
    D3DVIEWPORT9 oldVp = {};
    if (FAILED(dev->GetRenderTarget(0, &oldRt)) || !oldRt || FAILED(dev->GetViewport(&oldVp))) {
        if (oldRt) oldRt->Release(); return;
    }
    DWORD scissorOn = FALSE;
    RECT oldScissor = { 0, 0, (LONG)g_bbW, (LONG)g_bbH };
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorOn);
    if (scissorOn) dev->GetScissorRect(&oldScissor);

    float hudScale = 1.0f, hudOffX = 0.0f, hudOffY = 0.0f;
    OLProxy_GetHudFit(&hudScale, &hudOffX, &hudOffY);
    // Texture-less overlay draws are Outlast's full-screen fades/darkening passes.  Keep those
    // edge-to-edge; only actual textured UI should use the comfortable menu/HUD fit.
    if (!fitUi) { hudScale = 1.0f; hudOffX = 0.0f; hudOffY = 0.0f; }
    if (hudScale < 0.40f) hudScale = 0.40f;
    if (hudScale > 1.00f) hudScale = 1.00f;
    UINT drawW = (UINT)((float)g_nativeEyeW * hudScale);
    UINT drawH = (UINT)((float)g_nativeEyeH * hudScale);
    if (drawW < 16) drawW = 16;
    if (drawH < 16) drawH = 16;
    int localX = ((int)g_nativeEyeW - (int)drawW) / 2 + (int)((float)g_nativeEyeW * hudOffX);
    int localY = ((int)g_nativeEyeH - (int)drawH) / 2 + (int)((float)g_nativeEyeH * hudOffY);
    if (localX < 0) localX = 0;
    if (localY < 0) localY = 0;
    if (localX + (int)drawW > (int)g_nativeEyeW) localX = (int)g_nativeEyeW - (int)drawW;
    if (localY + (int)drawH > (int)g_nativeEyeH) localY = (int)g_nativeEyeH - (int)drawH;

    static volatile LONG s_loggedNativeUiFit = 0;
    if (fitUi && InterlockedCompareExchange(&s_loggedNativeUiFit, 1, 0) == 0)
        Log("[OLVR][NATIVEEYE] native menu/HUD fit active: %.0f%% offset=(%.0f%%,%.0f%%) viewport=%ux%u",
            hudScale * 100.0f, hudOffX * 100.0f, hudOffY * 100.0f, drawW, drawH);

    if (SUCCEEDED(o_SetRenderTarget(dev, 0, g_nativeStereoRT))) {
        for (int eye = 0; eye < 2; ++eye) {
            D3DVIEWPORT9 vp = oldVp;
            vp.X = eye * g_nativeEyeW + (UINT)localX;
            vp.Y = (UINT)localY;
            vp.Width = drawW;
            vp.Height = drawH;
            dev->SetViewport(&vp);
            if (scissorOn) {
                const float sx = g_bbW ? (float)drawW / (float)g_bbW : 1.0f;
                const float sy = g_bbH ? (float)drawH / (float)g_bbH : 1.0f;
                RECT sr;
                sr.left = (LONG)vp.X + (LONG)((float)oldScissor.left * sx);
                sr.top = (LONG)vp.Y + (LONG)((float)oldScissor.top * sy);
                sr.right = (LONG)vp.X + (LONG)((float)oldScissor.right * sx);
                sr.bottom = (LONG)vp.Y + (LONG)((float)oldScissor.bottom * sy);
                if (sr.left < (LONG)vp.X) sr.left = (LONG)vp.X;
                if (sr.top < (LONG)vp.Y) sr.top = (LONG)vp.Y;
                if (sr.right > (LONG)(vp.X + vp.Width)) sr.right = (LONG)(vp.X + vp.Width);
                if (sr.bottom > (LONG)(vp.Y + vp.Height)) sr.bottom = (LONG)(vp.Y + vp.Height);
                dev->SetScissorRect(&sr);
            }
            draw();
        }
        o_SetRenderTarget(dev, 0, oldRt);
    }
    dev->SetViewport(&oldVp);
    if (scissorOn) dev->SetScissorRect(&oldScissor);
    oldRt->Release();
}

static bool UiDrawState() {
    return g_rtIsBackbuffer && g_rsZ == D3DZB_FALSE && g_rsBlend != 0 && g_rsDest == D3DBLEND_INVSRCALPHA;
}

extern "C" int  OLProxy_GetUiMirror() { return g_uiMirror; }
extern "C" void OLProxy_SetUiMirror(int v) {
    if (v < 0 || v > 2) v = UIMIRROR_ON;
    g_uiMirror = v;
    if (v != UIMIRROR_MENUS) { g_uiAutoArmed = 0; g_uiQuietFrames = 0; }
}

void OLProxy_ToggleUiMirror() {
    LONG m = (g_uiMirror + 1) % 3;
    g_uiMirror = m;
    if (m != UIMIRROR_MENUS) { g_uiAutoArmed = 0; g_uiQuietFrames = 0; }
    static const char* kWhat[3] = {
        "OFF - UI drawn once across the frame (the split chops it: half per eye)",
        "ON - mirror every TEXTURED small-source UI draw (HUD + menus in both eyes)",
        "MENUS-ONLY - fallback: mirror only during menu-volume bursts (loses the in-game HUD)"
    };
    Log("[OLVR][UIMIRROR] mode %ld = %s (candidates seen=%ld, mirrored=%ld)", m, kWhat[m], g_uiSeen, g_uiMirrored);
}

// ---- [HUDFIT] pull the HUD into the part of the frame a headset can actually see -------------
// Requirement: the camcorder HUD (battery, record status, etc.) must stay readable when the
// camcorder is active. The camcorder HUD (battery, record dot, timecode) is drawn at the FRAME EDGES, and a headset
// never sees the edges of the frame - the lens cuts them off. The mirror already relocates each
// UI draw into a per-eye box (that is how it gets UI into both eyes), so the box is exactly the
// right place to shrink and nudge: scaling it pulls every HUD corner inward into the readable
// area, and the offsets let a specific element be centred by eye.
// Uniform scale keeps the HUD's own proportions - the ME1 lesson is that stretching UI to "fit"
// is what makes it look wrong. Percent-of-box units so it survives resolution changes.
// Touches the UI viewport ONLY: no scene draw, no eye offset, no projection. The stereo depth
// this build is kept for cannot be affected by anything in here.
static volatile LONG g_hudScalePct = 55;    // 40..100 - 100 = the old edge-to-edge behaviour; (Test47) 55: two players could not see the battery at 63
static volatile LONG g_hudOffXPct  = 0;     // -40..+40, percent of the eye's half width
static volatile LONG g_hudOffYPct  = 0;     // -40..+40, percent of the box height (+ = down)

extern "C" void OLProxy_GetHudFit(float* scale, float* offX, float* offY) {
    if (scale) *scale = (float)g_hudScalePct * 0.01f;
    if (offX)  *offX  = (float)g_hudOffXPct  * 0.01f;
    if (offY)  *offY  = (float)g_hudOffYPct  * 0.01f;
}
extern "C" void OLProxy_SetHudFit(float scale, float offX, float offY) {
    LONG s = (LONG)(scale * 100.0f + 0.5f);   if (s < 40) s = 40;  if (s > 100) s = 100;
    LONG x = (LONG)(offX  * 100.0f + (offX  < 0 ? -0.5f : 0.5f));
    LONG y = (LONG)(offY  * 100.0f + (offY  < 0 ? -0.5f : 0.5f));
    if (x < -40) x = -40; if (x > 40) x = 40;
    if (y < -40) y = -40; if (y > 40) y = 40;
    InterlockedExchange(&g_hudScalePct, s);
    InterlockedExchange(&g_hudOffXPct,  x);
    InterlockedExchange(&g_hudOffYPct,  y);
}

// [LENSFIT] (Test36) With each eye rendered through its own off-centre frustum, the middle of an
// eye's half is no longer straight ahead: on Quest the left eye's straight-ahead sits ~12% of the
// half to the right of centre and the right eye's ~12% to the left, both a little above centre.
// A UI box centred in each half would then sit in different directions for the two eyes (seen
// double). Each box is centred on its own eye's straight-ahead instead, so the HUD fuses dead
// ahead exactly as before.
extern "C" int OLUE3_LensFitLive();
extern "C" int OLXR_LensFitTangents(int half, float* l, float* r, float* u, float* d);

template <class F>
static HRESULT MirrorUiDraw(IDirect3DDevice9* dev, F&& draw) {
    if (g_bbW < 8 || OLUE3_StereoMode() != 1 || !OLUE3_SplitActive())
        return draw();
    // [CPUSPLIT] (Test36) This runs for EVERY draw call of both eyes. The texture classification
    // below is four COM calls into D3D9On12; the render-state test is free and rejects nearly every
    // scene draw, so it goes first (with the native-eye replay compiled out, nothing else needs tex).
    if (!kNativeEyeReplayEnabled && !UiDrawState()) return draw();

    UINT texW = 0, texH = 0;
    const UiTexVerdict tex = ClassifySourceTex(dev, &texW, &texH);
    const bool overlayState = g_rtIsBackbuffer && g_rsZ == D3DZB_FALSE && g_rsBlend != 0;
    if (kNativeEyeReplayEnabled && overlayState && tex == UITEX_FULLSCREEN)
        CaptureNativeSceneComposite(dev, draw);
    else if (kNativeEyeReplayEnabled && overlayState && (tex == UITEX_SMALL || tex == UITEX_NONE))
        ReplayNativeOverlay(dev, draw, tex == UITEX_SMALL);

    if (!UiDrawState()) return draw();
    if (tex == UITEX_NONE)       { InterlockedIncrement(&g_uiRejNoTex); return draw(); }  // fade/darken
    if (tex == UITEX_FULLSCREEN) { InterlockedIncrement(&g_uiRejFull);  return draw(); }  // composite/post

    D3DVIEWPORT9 vp;
    if (FAILED(dev->GetViewport(&vp))) return draw();
    LONG n = InterlockedIncrement(&g_uiSeen);
    // Count BEFORE deciding: mode 2's volume gate needs the signal even while it is disarmed.
    if (!UiMirrorActiveNow()) { InterlockedIncrement(&g_uiFrameHits); return draw(); }
    // PER-FRAME, reset at Present. The first version counted per SESSION and tripped at 4001,
    // which is simply a minute of ordinary UI drawing - it disarmed a mirror that was working.
    // A real cascade shows up as hundreds of catches in a SINGLE frame.
    LONG f = InterlockedIncrement(&g_uiFrameHits);
    if (f > 600) {
        InterlockedExchange(&g_uiMirror, UIMIRROR_OFF);
        InterlockedExchange(&g_uiAutoArmed, 0);
        Log("[OLVR][UIMIRROR] SAFETY OFF: %ld candidate draws in ONE frame - the classifier is "
            "catching non-UI draws. Mirror disarmed, everything else untouched.", f);
        return draw();
    }
    // Texture dimensions are in here because they are the classifier's whole basis now: if an
    // overlay ever slips through again, this line names it.
    if (n <= 12)
        Log("[OLVR][UIMIRROR] candidate #%ld tex=%ux%u vp=%ux%u@%u,%u bb=%ux%u zEnable=%ld blend=%ld dest=%ld",
            n, texW, texH, vp.Width, vp.Height, vp.X, vp.Y, g_bbW, g_bbH, g_rsZ, g_rsBlend, g_rsDest);

    float fx = 0.0f, fy = 0.0f;
    OLUE3_GetEyeFov(&fx, &fy);
    const UINT halfW = g_bbW / 2;
    UINT boxH = g_bbH;
    if (fx > 0.1f && fy > 0.1f) {
        const float uiAspect = (float)g_bbW / (float)g_bbH;
        float h = (float)g_bbH * tanf(fx * 0.5f) / (tanf(fy * 0.5f) * uiAspect);
        if (h < g_bbH * 0.2f) h = g_bbH * 0.2f;
        if (h > (float)g_bbH)  h = (float)g_bbH;
        boxH = (UINT)h;
    }
    const UINT boxY = vp.Y + ((g_bbH > boxH) ? (g_bbH - boxH) / 2 : 0);

    // [HUDFIT] shrink the box and nudge it, so edge-drawn HUD lands where the lens can see it.
    const float hs = (float)g_hudScalePct * 0.01f;
    UINT drawW = (UINT)((float)halfW * hs); if (drawW < 16) drawW = 16;
    UINT drawH = (UINT)((float)boxH  * hs); if (drawH < 16) drawH = 16;
    const int padX = ((int)halfW - (int)drawW) / 2 + (int)((float)halfW * (float)g_hudOffXPct * 0.01f);
    const int padY = ((int)boxH  - (int)drawH) / 2 + (int)((float)boxH  * (float)g_hudOffYPct * 0.01f);

    // [LENSFIT] straight-ahead of each half, in pixels from the half's centre (+ right / + down).
    int lensDx[2] = { 0, 0 }, lensDy = 0;
    if (OLUE3_LensFitLive()) {
        for (int e = 0; e < 2; ++e) {
            float l, r, u, d;
            if (OLXR_LensFitTangents(e, &l, &r, &u, &d)) {
                lensDx[e] = (int)(-(r + l) / (r - l) * 0.5f * (float)halfW);
                if (e == 0) lensDy = (int)((u + d) / (u - d) * 0.5f * (float)g_bbH);
            }
        }
    }

    // Clamp into the backbuffer vertically, and into THIS EYE'S OWN HALF horizontally - a
    // viewport outside the render target is a silent no-draw, and one that crosses the seam
    // would put this eye's HUD into the other eye's picture.
    int yy = (int)boxY + padY + lensDy;
    if (yy < 0) yy = 0;
    if (yy + (int)drawH > (int)g_bbH) yy = (int)g_bbH - (int)drawH;

    D3DVIEWPORT9 half = vp;
    half.Y = (UINT)yy; half.Height = drawH; half.Width = drawW;

    HRESULT hr = D3D_OK;
    for (int e = 0; e < 2; ++e) {
        const int lo = e ? (int)halfW : 0;
        int xx = lo + padX + lensDx[e];
        if (xx < lo) xx = lo;
        if (xx + (int)drawW > lo + (int)halfW) xx = lo + (int)halfW - (int)drawW;
        half.X = (UINT)xx;
        dev->SetViewport(&half);
        const HRESULT h = draw();
        if (e == 0) hr = h;
    }
    dev->SetViewport(&vp);
    InterlockedIncrement(&g_uiMirrored);
    return hr;
}

// [OCCLSKIP] (Test46) set while the game draws an occlusion query's test boxes (between its Issue BEGIN and END)
static volatile LONG g_inOcclQuery = 0;
static volatile LONG g_occlSkip = 1;              // with OcclForce: skip those draws and never wait for answers
static volatile LONG g_occlSkippedDraws = 0;
extern "C" int OLUE3_OcclBypassOn();
static inline bool OcclDrawSkipped() {
    if (!g_inOcclQuery || !g_occlSkip || !OLUE3_OcclBypassOn()) return false;
    InterlockedIncrement(&g_occlSkippedDraws);
    return true;
}
static HRESULT STDMETHODCALLTYPE Hook_DrawPrimitive(IDirect3DDevice9* This, D3DPRIMITIVETYPE t, UINT s, UINT c) {
    if (OcclDrawSkipped()) return D3D_OK;
    return MirrorUiDraw(This, [&] { return o_DrawPrimitive(This, t, s, c); });
}
static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitive(IDirect3DDevice9* This, D3DPRIMITIVETYPE t,
        INT bv, UINT mi, UINT nv, UINT si, UINT pc) {
    if (OcclDrawSkipped()) return D3D_OK;
    return MirrorUiDraw(This, [&] { return o_DrawIndexedPrimitive(This, t, bv, mi, nv, si, pc); });
}
static HRESULT STDMETHODCALLTYPE Hook_DrawPrimitiveUP(IDirect3DDevice9* This, D3DPRIMITIVETYPE t,
        UINT pc, const void* d, UINT stride) {
    if (OcclDrawSkipped()) return D3D_OK;
    return MirrorUiDraw(This, [&] { return o_DrawPrimitiveUP(This, t, pc, d, stride); });
}
static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitiveUP(IDirect3DDevice9* This, D3DPRIMITIVETYPE t,
        UINT mvi, UINT nv, UINT pc, const void* id, D3DFORMAT idf, const void* vd, UINT stride) {
    if (OcclDrawSkipped()) return D3D_OK;
    return MirrorUiDraw(This, [&] { return o_DrawIndexedPrimitiveUP(This, t, mvi, nv, pc, id, idf, vd, stride); });
}

static void* HookSlot(void** vtbl, int index, void* hook) {
    DWORD prot = 0;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &prot);
    void* orig = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void*), prot, &prot);
    return orig;
}

static void RunWindowFixIfDue();       // [SCREQ] window lever, defined with Hook_Reset below
static void RunFullscreenFixIfDue();   // [FSFIX] fullscreen re-assert, defined with Hook_Reset below
static void HiResNudge(long presentCount);   // [HIRES] defined with Hook_Reset below
extern "C" double OLXR_PerfNowMs();
extern "C" void   OLXR_PerfNotePresent(double ms);
extern "C" void   OLXR_OnDeviceLost();

static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* This, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    LONG n = InterlockedIncrement(&g_presentCount);
    if (n == 1) {
        Log("[OLVR] first Present - frame loop is live");
        Log("[OLVR][PERF28] direct final-backbuffer capture active; expensive 3840x1080 native-eye replay disabled");
    }
    if (n == 120) Log("[OLVR] 120 presents - hook stable");
    HiResNudge(n);   // [HIRES]
    g_uiFrameHitsLast = InterlockedExchange(&g_uiFrameHits, 0);   // per-frame UI catch count
    UiMirrorAutoTick(g_uiFrameHitsLast);
    RunWindowFixIfDue();
    RunFullscreenFixIfDue();
    // FIRST RUN: open the menu once, by itself, so the player discovers it exists. Waiting a few
    // hundred frames keeps it out of the startup logos and lands it on the title screen, where
    // the game is drawing normally and pausing costs nothing.
    if (g_firstRunPending && n >= kFirstRunOpenAtPresent) {
        g_firstRunPending = 0;
        if (!OLMenu_Visible()) { OLMenu_Toggle(); Log("[OLVR][MENU] opened for the first run"); }
    }
    if ((n % 600) == 0)
        Log("[OLVR][UIMIRROR] alive: %ld candidates last frame, %ld mirrored total, passed through: "
            "%ld untextured (fades/darkening) + %ld scene-sized (mode=%ld armed=%ld)",
            g_uiFrameHitsLast, g_uiMirrored, g_uiRejNoTex, g_uiRejFull, g_uiMirror, g_uiAutoArmed);
    // The menu is drawn into the BACKBUFFER before the XR capture reads it, so it rides along
    // into the headset with the rest of the frame - no second layer, no separate pose, and it
    // lands in both eyes because it draws itself into both halves.
    __try { OLMenu_Render(This, g_bbW, g_bbH); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("[OLVR][MENU] EXCEPTION while drawing - menu skipped"); }
    __try { OLDev_OnPresent(This, n); }   // [DEVBRIDGE] no-op unless enabled
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("[OLVR][DEVBRIDGE] EXCEPTION - skipped"); }
    if (g_xrAllowed && g_xrEnabled) {
        __try { OLXR_OnPresent(This); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&g_xrEnabled, 0);
            Log("[OLVR] EXCEPTION in XR submit - XR disabled, game continues flat");
        }
    }
    // The pair belongs to the frame that was just submitted. The next frame must earn a fresh
    // pair; otherwise a menu/transition frame with only one composite could mix two moments.
    if (kNativeEyeReplayEnabled && OLUE3_SplitActive() && g_nativeCompositeCount != 2) ++g_nativeMisses;
    InterlockedExchange(&g_nativeCompositeCount, 0);
    InterlockedExchange(&g_nativePairValid, 0);
    if (kNativeEyeReplayEnabled && (n % 600) == 0)
        Log("[OLVR][NATIVEEYE] alive: full-res pairs=%ld incomplete frames=%ld",
            g_nativePairs, g_nativeMisses);
    // [MIRRORFPS] (Test46) The headset has had this frame (OLXR_OnPresent above). Showing it on the monitor
    // too - the one-eye blow-up plus a windowed Present of the whole 3456x1824 backbuffer - cost the game's
    // render thread, its bottleneck, about a millisecond EVERY frame. The monitor now gets at most
    // [Render] DesktopMirrorFps frames a second (0 = every frame); a skipped Present still flushes the
    // command buffer, so the GPU starts on the frame as before. Menus, loading and the theater (no stereo
    // split) present every frame.
    const bool split = OLUE3_SplitActive() && OLUE3_StereoMode() == 1;
    const LONG mfps = g_mirrorFps;
    if (split && mfps > 0 && g_xrAllowed && g_xrEnabled) {
        static double s_lastMs = 0.0;
        const double now = OLXR_PerfNowMs();
        if (now - s_lastMs < 1000.0 / (double)mfps - 1.0) {
            static IDirect3DQuery9* s_flush = nullptr;
            if (!s_flush) This->CreateQuery(D3DQUERYTYPE_EVENT, &s_flush);
            if (s_flush) { s_flush->Issue(D3DISSUE_END); s_flush->GetData(nullptr, 0, D3DGETDATA_FLUSH); }
            InterlockedIncrement(&g_mirrorSkipped);
            return D3D_OK;
        }
        s_lastMs = now;
    }
    // XR consumed the untouched SBS frame above. Change only what the monitor records.
    DrawOneEyeDesktopMirror();
    const double tp = OLXR_PerfNowMs();
    const HRESULT hr = o_Present(This, a, b, c, d);
    OLXR_PerfNotePresent(OLXR_PerfNowMs() - tp);   // [PERF] a blocking Present = GPU-bound
    return hr;
}

// [SCREQ] the WINDOW lever. Live evidence 2026-07-22: the post-intro windowed size tracks the
// window's real client rect - GetMonitorInfo was spoofed AND GetAdapterDisplayMode never called
// (spoofs=0), yet the game asked exactly its monitor's 2560x1440. Windows itself clamps the
// window; the engine just measures it. So change what it measures: after the game's own windowed
// Reset comes up short, resize its window to the requested client size (overhanging the desktop
// is fine - the headset consumes the backbuffer, not the screen). If UE3 follows its client rect
// it re-Resets ITSELF to the request and the buffer stays honest; if it doesn't, nothing breaks.
// Two attempts max so a game that fights back doesn't cause a resize war.
static HWND g_gameWnd = nullptr;
static LONG g_winFixAtPresent = 0;     // present # to fire at (0 = disarmed)
static LONG g_winFixTries = 0;
extern "C" int OLUE3_HiResActive();
extern "C" int OLUE3_HiResApplied();
extern "C" float OLUE3_GetRenderScale();
static void ArmWindowFix() {
    if (OLUE3_HiResActive()) return;   // [HIRES] the engine is sized from inside now; never fight the window
    if (!g_gameWnd || g_winFixTries >= 2) return;
    g_winFixAtPresent = g_presentCount + 30;   // let the Reset settle first
}
static void RunWindowFixIfDue() {
    if (!g_winFixAtPresent || g_presentCount < g_winFixAtPresent) return;
    g_winFixAtPresent = 0;
    ++g_winFixTries;
    // A framed window larger than the desktop is clamped before UE3 measures it. Make the VR
    // render window borderless so its CLIENT area can genuinely exceed the monitor. The desktop
    // mirror may overhang; OpenXR still receives the complete backbuffer.
    const LONG_PTR style = GetWindowLongPtrW(g_gameWnd, GWL_STYLE);
    const LONG_PTR clean = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                                      WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_POPUP | WS_VISIBLE;
    SetWindowLongPtrW(g_gameWnd, GWL_STYLE, clean);
    SetWindowPos(g_gameWnd, nullptr, 0, 0, g_reqW, g_reqH,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | SWP_FRAMECHANGED);
    // SetWindowPos normally produces WM_SIZE, but explicitly posting it makes the UE3 viewport
    // update deterministic on systems where the asynchronous move is coalesced by DWM.
    if (g_reqW <= 0xFFFF && g_reqH <= 0xFFFF) {
        PostMessageW(g_gameWnd, WM_SIZE, SIZE_RESTORED,
                     MAKELPARAM((WORD)g_reqW, (WORD)g_reqH));
        PostMessageW(g_gameWnd, WM_EXITSIZEMOVE, 0, 0);
    }
    Log("[OLVR][SCREQ] window fix #%ld: moved game window to primary at client %ldx%ld - "
        "if the engine follows its client rect, the NEXT Reset line is the verdict",
        g_winFixTries, g_reqW, g_reqH);
}

// [FSFIX] re-assert exclusive fullscreen 4K after the VR compositor knocks the game windowed.
// THE FINDING (2026-07-23, from the SCRDIAG log): the game boots 3840x2160 EXCLUSIVE FULLSCREEN
// and that is TRUE 4K in the headset (eye 1728x2160, confirmed). When the XR session starts the
// game loses exclusive fullscreen (focus/display grab) and UE3 falls back to WINDOWED, where the
// backbuffer is DPI/work-area-capped at 3633x2044 no matter what the window is set to.
// Re-taking fullscreen from the video menu restores true 4K and it STICKS - so the fix is to
// make the game re-run ITS OWN fullscreen mode change once the compositor has settled.
//
// The trigger is ALT+ENTER: UE3's viewport window proc toggles fullscreen on WM_SYSKEYDOWN
// VK_RETURN with the ALT context bit. That runs the ENGINE's mode change, so the scene resizes
// to the fullscreen resolution - NO forced backbuffer, so none of the cross-eyed 4.11 risk. It
// is exactly what the video menu does. If UE3 does not handle it, nothing happens (safe).
static volatile LONG g_isWindowed   = 0;
static volatile LONG g_autoFs       = 1;     // outlastvr.ini [Render] AutoFullscreen (default on)
static LONG g_fsFixAtPresent = 0;            // present # to attempt at (0 = disarmed)
static LONG g_fsFixCount     = 0;            // attempts since the last windowed drop
// A desktop-sized exclusive mode is the wrong answer for VR supersampling: on a 1920x1080
// monitor it hard-caps the SBS image to 960x1080 per eye.  Keep the device windowed and make
// the borderless client larger than the desktop instead.  D3D9 permits that, and UE3 receives
// the WM_SIZE so its scene viewport, UI and backbuffer all change together.
static volatile LONG g_highResWindow = 1;
static LONG g_leaveExclusiveAtPresent = 0;

static void SendFullscreenToggle(const char* why) {
    if (!g_gameWnd) { Log("[OLVR][FSFIX] no game window yet - cannot toggle"); return; }
    // WM_SYSKEYDOWN/UP for VK_RETURN. lParam bit 29 = ALT context, bits 16-23 = Enter scan 0x1C.
    const LPARAM downL = (LPARAM)(0x00000001u | (0x1Cu << 16) | (1u << 29));
    const LPARAM upL   = (LPARAM)(0x00000001u | (0x1Cu << 16) | (1u << 29) | (1u << 30) | (1u << 31));
    PostMessageW(g_gameWnd, WM_SYSKEYDOWN, VK_RETURN, downL);
    PostMessageW(g_gameWnd, WM_SYSKEYUP,   VK_RETURN, upL);
    ++g_fsFixCount;
    Log("[OLVR][FSFIX] ALT+ENTER -> game (%s) attempt #%ld - the win is a 'Reset -> 3840x2160 windowed=0' line next",
        why, g_fsFixCount);
}

// [HIRES] If the viewport was created before the hook (or the headset asked for a new size), make
// the engine re-run its own resize: its WM_SIZE handler re-reads the client rect, finds it differs
// from the viewport, and calls Resize -> UpdateViewportRHI -> our size.
static void HiResNudge(long n) {
    if (g_gameWnd && OLUE3_HiResActive() && g_isWindowed && !OLUE3_HiResApplied() && (n % 90) == 45) {
        PostMessageW(g_gameWnd, WM_SIZE, SIZE_RESTORED, 0);
        Log("[OLVR][HIRES] nudging the engine to resize (present %ld)", n);
    }
}

static void RunFullscreenFixIfDue() {
    if ((g_highResWindow || OLUE3_HiResActive()) && g_leaveExclusiveAtPresent &&
        g_presentCount >= g_leaveExclusiveAtPresent) {
        g_leaveExclusiveAtPresent = 0;
        if (!g_isWindowed) {
            SendFullscreenToggle("leave monitor-sized exclusive mode for sharp VR window");
        } else {
            ArmWindowFix();
        }
    }
    if (OLUE3_HiResActive()) return;   // [HIRES] windowed is the point; never re-assert fullscreen
    if (!g_autoFs || !g_fsFixAtPresent || g_presentCount < g_fsFixAtPresent) return;
    g_fsFixAtPresent = 0;
    if (!g_isWindowed) return;             // already fullscreen: nothing to do
    if (g_fsFixCount >= 3) {               // 3 tries is enough; leave the manual chord as backup
        Log("[OLVR][FSFIX] gave up after 3 auto attempts - the picture will be softer than it could be");
        return;
    }
    SendFullscreenToggle("auto, windowed after the compositor settled");
    g_fsFixAtPresent = g_presentCount + 600;   // still windowed a few seconds later? try once more
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pp) {
    if (pp) {
        Log("[OLVR] Reset -> %ux%u windowed=%d (monitor-rect spoofs=%ld, display-mode spoofs=%ld)",
            pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->Windowed, g_dispqHits, g_admHits);
        // The 2026-07-22 build FORCED pp up to the ini request here. That was wrong by
        // construction: the engine sizes its scene viewport from the resolution IT asked for, so
        // it rendered 2560x1440 into the corner of the forced 3840x2160 buffer and the XR
        // half-split cut through the middle of the right eye's view - cross-eyed garbage in the
        // headset. The buffer must always equal what the engine believes; to get a BIGGER buffer,
        // make the ENGINE ask bigger (the GetAdapterDisplayMode spoof) - never resize it behind
        // the engine's back.
        g_isWindowed = pp->Windowed ? 1 : 0;
        if (pp->hDeviceWindow) g_gameWnd = pp->hDeviceWindow;
        // [HIRES] Exclusive fullscreen can only be a real display mode (the desktop). Ask the
        // game itself to go windowed once (its own ALT+ENTER), where the big render can exist.
        if (!pp->Windowed && OLUE3_HiResActive() && !g_leaveExclusiveAtPresent) {
            g_leaveExclusiveAtPresent = g_presentCount + 120;
            Log("[OLVR][HIRES] exclusive %ux%u - asking the game to go windowed for the high-res render",
                pp->BackBufferWidth, pp->BackBufferHeight);
        }

        if (!pp->Windowed && g_highResWindow && g_spoofArmed &&
            ((LONG)pp->BackBufferWidth < g_reqW || (LONG)pp->BackBufferHeight < g_reqH)) {
            // Do not let a 1080p desktop silently win over a 4K VR request.  We do NOT enlarge
            // this Reset behind UE3's back (that produced the old split/cross-eyed failure).
            // Let this coherent engine-sized Reset complete, then ask UE3 itself to leave
            // exclusive mode and resize its borderless client to the requested dimensions.
            g_leaveExclusiveAtPresent = g_presentCount + 120;
            Log("[OLVR][SCREQ] undersized exclusive %ux%u detected - scheduling coherent "
                "borderless %ldx%ld transition", pp->BackBufferWidth, pp->BackBufferHeight,
                g_reqW, g_reqH);
        } else if (!pp->Windowed) {
            // Back in exclusive fullscreen (either the boot mode or the re-assert took): disarm
            // the fullscreen fix and reset its budget for the next time the compositor drops it.
            g_fsFixAtPresent = 0; g_fsFixCount = 0;
        } else if (g_highResWindow && g_spoofArmed &&
                   ((LONG)pp->BackBufferWidth < g_reqW || (LONG)pp->BackBufferHeight < g_reqH)) {
            Log("[OLVR][SCREQ] windowed Reset %ux%u is below request %ldx%ld - resizing the "
                "engine-owned client", pp->BackBufferWidth, pp->BackBufferHeight, g_reqW, g_reqH);
            ArmWindowFix();
        } else if (g_autoFs) {
            // Windowed = the compositor knocked the game out of fullscreen. Re-assert it once
            // the compositor has settled (~a few seconds of presents). The window lever is the
            // OLD strategy and only ever reached 3633x2044; fullscreen is strictly better, so
            // the window is NOT also fought when AutoFullscreen is on.
            if (!g_fsFixAtPresent && g_fsFixCount < 3)
                g_fsFixAtPresent = g_presentCount + 300;
            Log("[OLVR][SCREQ] game went windowed %ux%u - AutoFullscreen armed (will re-assert 4K fullscreen)",
                pp->BackBufferWidth, pp->BackBufferHeight);
        } else if (g_spoofArmed &&
                   ((LONG)pp->BackBufferWidth < g_reqW || (LONG)pp->BackBufferHeight < g_reqH)) {
            Log("[OLVR][SCREQ] game asked %ux%u (< request %ldx%ld) - staying coherent at the "
                "game's size and trying the window lever",
                pp->BackBufferWidth, pp->BackBufferHeight, g_reqW, g_reqH);
            ArmWindowFix();
        }
        g_bbW = pp->BackBufferWidth; g_bbH = pp->BackBufferHeight;
    }
    if (pp && g_xrAllowed && !g_keepVsync && pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        Log("[OLVR][PACING] Reset PresentationInterval 0x%08lX -> IMMEDIATE", pp->PresentationInterval);
        pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    if (g_leftRT) { g_leftRT->Release(); g_leftRT = nullptr; g_leftW = g_leftH = 0; }  // DEFAULT pool dies on Reset
    if (g_desktopMirrorRT) {
        g_desktopMirrorRT->Release(); g_desktopMirrorRT = nullptr;
        g_desktopMirrorW = g_desktopMirrorH = 0;
    }
    if (g_nativeStereoRT) {
        g_nativeStereoRT->Release(); g_nativeStereoRT = nullptr;
        g_nativeEyeW = g_nativeEyeH = 0; g_nativeFmt = D3DFMT_UNKNOWN;
    }
    InterlockedExchange(&g_nativeCompositeCount, 0);
    InterlockedExchange(&g_nativePairValid, 0);
    OLMenu_OnDeviceLost();   // the menu's ImGui/RT objects are DEFAULT pool too - drop before Reset
    OLXR_OnDeviceLost();     // [ASYNCREAD] the readback staging targets are DEFAULT pool as well
    HRESULT hr = o_Reset(This, pp);
    if (SUCCEEDED(hr)) OLMenu_OnDeviceReset();
    return hr;
}

// ---- [GAMMA] SetGammaRamp capture ----------------------------------------------------------
// Outlast's brightness calibration ("adjust until the logo is barely visible") works through
// IDirect3DDevice9::SetGammaRamp, which reprograms the MONITOR's output curve. This capture
// reads the backbuffer BEFORE that curve is applied, so the headset never saw that
// brightness calibration - one of the two halves of "the colors are weird / blacks are not good"
// (the other is the sRGB-vs-2.2 decode mismatch, handled by the gamma LUT in ol_xr.cpp).
// Capture the ramp here; ol_xr.cpp folds it into its blit LUT so headset == calibrated TV.
typedef void (STDMETHODCALLTYPE *SetGammaRamp_t)(IDirect3DDevice9*, UINT, DWORD, const D3DGAMMARAMP*);
static SetGammaRamp_t o_SetGammaRamp = nullptr;
static D3DGAMMARAMP  g_gammaRamp = {};
static volatile long g_gammaRampGen = 0;    // 0 = none/identity; bumps on every non-identity set

extern "C" int OLProxy_GammaRampGen() { return g_gammaRampGen; }
extern "C" int OLProxy_GetGammaRamp(unsigned short* r, unsigned short* g, unsigned short* b) {
    if (!g_gammaRampGen || !r || !g || !b) return 0;
    memcpy(r, g_gammaRamp.red,   256 * sizeof(unsigned short));
    memcpy(g, g_gammaRamp.green, 256 * sizeof(unsigned short));
    memcpy(b, g_gammaRamp.blue,  256 * sizeof(unsigned short));
    return g_gammaRampGen;
}

static void STDMETHODCALLTYPE Hook_SetGammaRamp(IDirect3DDevice9* This, UINT sc, DWORD flags, const D3DGAMMARAMP* ramp) {
    if (ramp) {
        // Identity = value[i] == i*257 (0..65535 over 256 entries). Tolerate rounding slop.
        bool identity = true;
        for (int i = 0; i < 256 && identity; ++i) {
            const int want = i * 257;
            if (abs((int)ramp->red[i]   - want) > 384 ||
                abs((int)ramp->green[i] - want) > 384 ||
                abs((int)ramp->blue[i]  - want) > 384) identity = false;
        }
        if (!identity) {
            memcpy(&g_gammaRamp, ramp, sizeof(g_gammaRamp));
            LONG gen = InterlockedIncrement(&g_gammaRampGen);
            if (gen <= 3)
                Log("[OLVR][GAMMA] game gamma ramp captured (gen=%ld, mid red=%u) - folded into the headset blit",
                    gen, (unsigned)ramp->red[128]);
        } else if (g_gammaRampGen) {
            InterlockedExchange(&g_gammaRampGen, 0);   // game reset to identity
            Log("[OLVR][GAMMA] game gamma ramp reset to identity");
        }
    }
    if (o_SetGammaRamp) o_SetGammaRamp(This, sc, flags, ramp);
}

// ---- hardware occlusion queries -------------------------------------------------------
// Third hypothesis in the same-frame flicker investigation (tracker: never called; precomputed visibility:
// bypassed live, no change). The remaining per-object visibility machine is D3D9 hardware
// occlusion queries: the engine draws bounding boxes, asks the GPU how many pixels passed,
// and culls meshes whose LAST answer was 0. The query handles come from a pool shared per
// scene and recycled per frame - with TWO views per frame the second view can fetch results
// that belong to other queries, so its verdicts are garbage and props strobe. One view per
// frame (AER) never contends the pool, which matches AER being clean.
//
// This module IS d3d9.dll, so no exe fingerprint is needed: hook CreateQuery (device vtable 118),
// then GetData (query vtable 7 - one vtable shared by every query). While the bypass
// is ON every occlusion result becomes "lots of pixels visible" and NOTHING is ever culled
// by a query, corrupt or not. Costs the culling perf; answers the question.
extern "C" int OLUE3_OcclBypassOn();   // the OcclForce flag, lives in ol_ue3.cpp
static CreateQuery_t  o_CreateQuery = nullptr;
static QueryGetData_t o_QueryGetData = nullptr;
static volatile LONG g_queryVtblHooked = 0;
static LONG g_hwqCreated = 0, g_hwqForced = 0;
extern "C" void OLProxy_HwqStats(long* created, long* forced) {
    if (created) *created = g_hwqCreated;
    if (forced)  *forced = g_hwqForced;
}

typedef HRESULT (STDMETHODCALLTYPE *QueryIssue_t)(IDirect3DQuery9*, DWORD);
static QueryIssue_t o_QueryIssue = nullptr;
// [OCCLSKIP] (Test46) With OcclForce every occlusion answer is replaced by "visible" anyway, yet the game still
// drew each query's test boxes (twice a frame - both eyes) and its render thread waited for the GPU to answer
// (UE3 reads last frame's queries with wait=TRUE) - work and stalls for answers that were thrown away. Now the
// boxes are not drawn and the answer comes back at once. [Render] OcclusionSkip=0 restores the old way.
static HRESULT STDMETHODCALLTYPE Hook_QueryIssue(IDirect3DQuery9* This, DWORD flags) {
    if (This->GetType() == D3DQUERYTYPE_OCCLUSION) {
        if (flags & D3DISSUE_BEGIN) InterlockedExchange(&g_inOcclQuery, 1);
        else if (flags & D3DISSUE_END) InterlockedExchange(&g_inOcclQuery, 0);
    }
    return o_QueryIssue(This, flags);
}
static HRESULT STDMETHODCALLTYPE Hook_QueryGetData(IDirect3DQuery9* This, void* pData, DWORD size, DWORD flags) {
    if (g_occlSkip && OLUE3_OcclBypassOn() && This->GetType() == D3DQUERYTYPE_OCCLUSION) {
        if (pData && size >= sizeof(DWORD)) *(DWORD*)pData = 0x7FFFFF;
        InterlockedIncrement(&g_hwqForced);
        return S_OK;
    }
    HRESULT hr = o_QueryGetData(This, pData, size, flags);
    if (hr == S_OK && pData && size >= sizeof(DWORD) && OLUE3_OcclBypassOn() &&
        This->GetType() == D3DQUERYTYPE_OCCLUSION) {
        *(DWORD*)pData = 0x7FFFFF;          // "many pixels" -> the engine never culls on it
        InterlockedIncrement(&g_hwqForced);
    }
    return hr;
}

extern "C" void OLProxy_SetOcclSkip(int on) { InterlockedExchange(&g_occlSkip, on ? 1 : 0); Log("[OLVR][OCCLSKIP] %s", on ? "ON - forced occlusion queries draw nothing and never wait" : "OFF"); }
extern "C" long OLProxy_OcclSkippedDraws() { return InterlockedExchange(&g_occlSkippedDraws, 0); }
static HRESULT STDMETHODCALLTYPE Hook_CreateQuery(IDirect3DDevice9* This, D3DQUERYTYPE type, IDirect3DQuery9** ppQ) {
    HRESULT hr = o_CreateQuery(This, type, ppQ);
    if (SUCCEEDED(hr) && ppQ && *ppQ && type == D3DQUERYTYPE_OCCLUSION) {
        LONG n = InterlockedIncrement(&g_hwqCreated);
        if (InterlockedCompareExchange(&g_queryVtblHooked, 1, 0) == 0) {
            void** qvt = *(void***)*ppQ;    // shared by all IDirect3DQuery9 instances
            o_QueryGetData = (QueryGetData_t)HookSlot(qvt, 7, (void*)&Hook_QueryGetData);
            o_QueryIssue = (QueryIssue_t)HookSlot(qvt, 6, (void*)&Hook_QueryIssue);   // [OCCLSKIP]
            Log("[OLVR][HWQ] occlusion-query GetData hooked via first query %p", (void*)*ppQ);
        }
        if (n == 1) Log("[OLVR][HWQ] first occlusion query created - hardware occlusion is in use");
    }
    return hr;
}

// ---- [D3D9EX] (Test36) the game on a D3D9Ex device --------------------------------------------
// Measured on this laptop at the asylum gate, publishing paused: native D3D9 78 fps, D3D9On12 46 -
// the 9On12 translation is the render thread's biggest cost. Native D3D9 cannot share a texture
// with the headset bridge, so every frame had to be read back (21 fps). D3D9Ex CAN share, but it
// has no MANAGED pool, which UE3 uses for nearly every texture and static mesh buffer (that is
// what crashed the earlier D3D9Ex attempt). So the device is created Ex and every MANAGED create
// is translated: vertex/index buffers to DEFAULT (still lockable), textures to DEFAULT + DYNAMIC
// (the only lockable DEFAULT textures). [XR] D3D9Ex=1 (the default); 0 falls back to D3D9On12,
// which is also what happens by itself if the Ex device cannot be created.
typedef HRESULT (STDMETHODCALLTYPE *CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *CreateVolumeTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *CreateCubeTexture_t)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *CreateVertexBuffer_t)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *CreateIndexBuffer_t)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
static CreateTexture_t       o_CreateTexture = nullptr;
static CreateVolumeTexture_t o_CreateVolumeTexture = nullptr;
static CreateCubeTexture_t   o_CreateCubeTexture = nullptr;
static CreateVertexBuffer_t  o_CreateVertexBuffer = nullptr;
static CreateIndexBuffer_t   o_CreateIndexBuffer = nullptr;
static volatile LONG g_d3d9Ex = 0;               // the game's device is D3D9Ex
static volatile LONG g_exManaged[3] = { 0, 0, 0 };   // translated: textures, buffers, failures
extern "C" int OLProxy_IsD3D9Ex() { return g_d3d9Ex ? 1 : 0; }
static void ExNoteFail(const char* what, HRESULT hr) {
    if (InterlockedIncrement(&g_exManaged[2]) <= 10) Log("[OLVR][D3D9EX] translated %s create FAILED hr=0x%08lX", what, hr);
}
// [LENSCAM] (Test46) the camcorder lens's render target (ol_ue3.cpp [LIVESCREEN]) is made by the game's
// render thread some time after the game thread asks for it: the next render target of the size asked
// for is it. Held (AddRef) until the next one replaces it; ol_xr.cpp copies it to the flip screen.
static volatile LONG g_lensWantW = 0, g_lensWantH = 0;
static IDirect3DTexture9* volatile g_lensTex = nullptr;
extern "C" void OLProxy_LensExpect(unsigned w, unsigned h) { InterlockedExchange(&g_lensWantH, (LONG)h); InterlockedExchange(&g_lensWantW, (LONG)w); }
extern "C" IDirect3DTexture9* OLProxy_LensTex() { return g_lensTex; }
// [EXSTAT] (Test47) "hitching quite a lot ... new room, loading, loading": what the game's resource uploads cost
// on the render thread under the MANAGED->DEFAULT translation - creates and locks, counted and timed, the
// longest named, reported with the PERF lines. Render thread only (UE3's RHI), so plain statics.
struct ExStat { long n; double sum, max; };
static ExStat g_esTexCreate = {}, g_esBufCreate = {}, g_esTexLock = {}, g_esTexRead = {}, g_esVbLock = {}, g_esIbLock = {};
static LARGE_INTEGER g_esFreq = {};
static inline double EsNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); if (!g_esFreq.QuadPart) QueryPerformanceFrequency(&g_esFreq); return (double)t.QuadPart * 1000.0 / (double)g_esFreq.QuadPart; }
static inline void EsAdd(ExStat& s, double ms) { ++s.n; s.sum += ms; if (ms > s.max) s.max = ms; }
typedef HRESULT (STDMETHODCALLTYPE *TexLockRect_t)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *BufLock_t)(void*, UINT, UINT, void**, DWORD);
static TexLockRect_t o_TexLockRect = nullptr;
static BufLock_t o_VbLock = nullptr, o_IbLock = nullptr;
// [TEXSHADOW] (Test47) The cure for the streaming hitches. Measured on a 1.5-minute walk through the admin block:
// the game's texture locks cost its render thread 7.4 s writing and 8.4 s reading (28,000 + 25,000 locks, up to
// 22 ms each), all in bursts as new areas stream in - 136 frames over 40 ms. Under the D3D9Ex translation the
// game's MANAGED textures were DEFAULT+DYNAMIC: memory the GPU draws from, slow for the CPU to map and very slow
// to read back. D3D9's own MANAGED pool kept a copy in system memory; this does the same: each translated
// texture is a plain DEFAULT texture (fastest to draw) carrying a SYSTEMMEM twin (private data, so it lives and
// dies with it). The game's locks go to the twin - ordinary RAM - and after a write lock UpdateTexture hands the
// level just written to the GPU (UpdateSurface, the locked rect), queued, nothing waits. Not UpdateTexture: it
// copies only regions D3D marked dirty, and it tracks dirt on level 0 only - mips streamed in one by one were
// never copied (first try: a black world). [XR] TextureShadow=0 restores the DYNAMIC translation.
static const GUID kTexShadowGuid = { 0x6f1a2c3e, 0x5b7d, 0x4e21, { 0x9a, 0x44, 0x1c, 0x3f, 0x8e, 0x52, 0xd7, 0x10 } };
static volatile LONG g_texShadow = 1;
static volatile LONG g_texShadowN = 0, g_texShadowFail = 0;
static int g_gpdAddRefs = -1;          // does GetPrivateData AddRef a D3DSPD_IUNKNOWN payload? measured once
typedef HRESULT (STDMETHODCALLTYPE *TexUnlockRect_t)(IDirect3DTexture9*, UINT);
static TexUnlockRect_t o_TexUnlockRect = nullptr;
static IDirect3DTexture9* ShadowOf(IDirect3DTexture9* t) {   // the twin, with a reference the caller releases
    IUnknown* u = nullptr; DWORD sz = sizeof(u);
    if (FAILED(t->GetPrivateData(kTexShadowGuid, &u, &sz)) || !u) return nullptr;
    if (g_gpdAddRefs == 0) u->AddRef();
    return (IDirect3DTexture9*)u;
}
// Every open lock of a shadowed texture level, until its unlock: the game's streamer locks many levels at once
// and unlocks them after the disk read lands (a 32-entry ring lost most of them - garbage on the GPU). An
// unlock with no record copies the whole level, to be safe.
struct TsLock { bool write; bool whole; RECT rc; };
static std::unordered_map<unsigned long long, TsLock> g_tsLocks;
static CRITICAL_SECTION g_tsCs; static volatile LONG g_tsCsInit = 0;
static inline unsigned long long TsKey(IDirect3DTexture9* t, UINT lv) { return ((unsigned long long)(uintptr_t)t << 5) ^ lv; }
static void TsCsEnsure() { if (InterlockedCompareExchange(&g_tsCsInit, 1, 0) == 0) InitializeCriticalSection(&g_tsCs); while (g_tsCsInit != 1) Sleep(0); }
static HRESULT STDMETHODCALLTYPE Hook_TexLockRect(IDirect3DTexture9* t, UINT lv, D3DLOCKED_RECT* lr, const RECT* rc, DWORD fl) {
    const bool read = (fl & D3DLOCK_READONLY) != 0;
    const double t0 = EsNow();
    HRESULT hr;
    IDirect3DTexture9* sys = ShadowOf(t);
    if (sys) {
        hr = o_TexLockRect(sys, lv, lr, rc, fl & ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE));
        if (SUCCEEDED(hr)) {
            TsLock e = {}; e.write = !read; e.whole = rc == nullptr; if (rc) e.rc = *rc;
            TsCsEnsure(); EnterCriticalSection(&g_tsCs); g_tsLocks[TsKey(t, lv)] = e; LeaveCriticalSection(&g_tsCs);
        }
        sys->Release();
    } else hr = o_TexLockRect(t, lv, lr, rc, fl);
    EsAdd(read ? g_esTexRead : g_esTexLock, EsNow() - t0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_TexUnlockRect(IDirect3DTexture9* t, UINT lv) {
    IDirect3DTexture9* sys = ShadowOf(t);
    if (!sys) return o_TexUnlockRect(t, lv);
    const HRESULT hr = o_TexUnlockRect(sys, lv);
    TsLock e = {}; bool known = false;
    TsCsEnsure(); EnterCriticalSection(&g_tsCs);
    { auto it = g_tsLocks.find(TsKey(t, lv)); if (it != g_tsLocks.end()) { e = it->second; known = true; g_tsLocks.erase(it); } }
    LeaveCriticalSection(&g_tsCs);
    if (!known) { e.write = true; e.whole = true; }
    if (e.write) {
        IDirect3DDevice9* dev = nullptr; IDirect3DSurface9* src = nullptr; IDirect3DSurface9* dst = nullptr;
        HRESULT u = E_FAIL;
        if (SUCCEEDED(t->GetDevice(&dev)) && dev && SUCCEEDED(sys->GetSurfaceLevel(lv, &src)) && SUCCEEDED(t->GetSurfaceLevel(lv, &dst))) {
            if (e.whole) u = dev->UpdateSurface(src, nullptr, dst, nullptr);
            else { const RECT r = e.rc; const POINT at = { r.left, r.top }; u = dev->UpdateSurface(src, &r, dst, &at); }
        }
        if (FAILED(u) && InterlockedIncrement(&g_texShadowFail) <= 5) Log("[OLVR][TEXSHADOW] UpdateSurface level %u FAILED hr=0x%08lX", lv, u);
        if (src) src->Release(); if (dst) dst->Release(); if (dev) dev->Release();
    }
    sys->Release();
    return hr;
}
// Make the DEFAULT texture + SYSTEMMEM twin; false = not possible here (the caller uses the DYNAMIC translation).
static bool TexShadowCreate(IDirect3DDevice9* d, UINT w, UINT h, UINT lv, DWORD use, D3DFORMAT fmt, IDirect3DTexture9** pp) {
    if (!g_texShadow || (use & (D3DUSAGE_AUTOGENMIPMAP | D3DUSAGE_DYNAMIC | D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) return false;
    IDirect3DTexture9* def = nullptr; IDirect3DTexture9* sys = nullptr;
    if (FAILED(o_CreateTexture(d, w, h, lv, use, fmt, D3DPOOL_DEFAULT, &def, nullptr)) || !def) return false;
    if (FAILED(o_CreateTexture(d, w, h, def->GetLevelCount(), 0, fmt, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys) { def->Release(); return false; }
    if (FAILED(def->SetPrivateData(kTexShadowGuid, sys, sizeof(IUnknown*), D3DSPD_IUNKNOWN))) { sys->Release(); def->Release(); return false; }
    if (g_gpdAddRefs < 0) {   // once: does GetPrivateData hand back an extra reference?
        IUnknown* u = nullptr; DWORD sz = sizeof(u);
        const ULONG before = (sys->AddRef(), sys->Release());
        if (SUCCEEDED(def->GetPrivateData(kTexShadowGuid, &u, &sz)) && u) {
            const ULONG after = (sys->AddRef(), sys->Release());
            g_gpdAddRefs = after > before ? 1 : 0;
            if (g_gpdAddRefs) u->Release();
            Log("[OLVR][TEXSHADOW] game textures get a system-memory copy (GetPrivateData %s)", g_gpdAddRefs ? "adds a reference" : "does not add a reference");
        } else g_gpdAddRefs = 0;
    }
    sys->Release();   // the private data holds it now
    if (!o_TexUnlockRect) {
        void** vt = *(void***)def;
        o_TexUnlockRect = (TexUnlockRect_t)HookSlot(vt, 20, (void*)&Hook_TexUnlockRect);
        if (!o_TexLockRect) o_TexLockRect = (TexLockRect_t)HookSlot(vt, 19, (void*)&Hook_TexLockRect);
    }
    InterlockedIncrement(&g_texShadowN);
    *pp = def;
    return true;
}
static HRESULT STDMETHODCALLTYPE Hook_VbLock(void* b, UINT o, UINT sz, void** pp, DWORD fl) {
    const double t0 = EsNow(); const HRESULT hr = o_VbLock(b, o, sz, pp, fl); EsAdd(g_esVbLock, EsNow() - t0); return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_IbLock(void* b, UINT o, UINT sz, void** pp, DWORD fl) {
    const double t0 = EsNow(); const HRESULT hr = o_IbLock(b, o, sz, pp, fl); EsAdd(g_esIbLock, EsNow() - t0); return hr;
}
extern "C" void OLProxy_ExStatReport() {
    char b[6][64]; const ExStat* all[6] = { &g_esTexCreate, &g_esBufCreate, &g_esTexLock, &g_esTexRead, &g_esVbLock, &g_esIbLock };
    for (int i = 0; i < 6; ++i) sprintf_s(b[i], "%ld %.1fms max %.1f", all[i]->n, all[i]->sum, all[i]->max);
    Log("[OLVR][EXSTAT] creates tex %s, buf %s | tex locks write %s, read %s | vb %s, ib %s | shadowed textures %ld",
        b[0], b[1], b[2], b[3], b[4], b[5], (long)g_texShadowN);
    g_esTexCreate = g_esBufCreate = g_esTexLock = g_esTexRead = g_esVbLock = g_esIbLock = ExStat{};
}
static HRESULT STDMETHODCALLTYPE Hook_CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT lv, DWORD use, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** pp, HANDLE* sh) {
    if (pool == D3DPOOL_MANAGED && g_d3d9Ex) {
        const double t0 = EsNow();
        if (pp && !sh && TexShadowCreate(d, w, h, lv, use, fmt, pp)) {   // (Test47) [TEXSHADOW]
            EsAdd(g_esTexCreate, EsNow() - t0); InterlockedIncrement(&g_exManaged[0]);
            return D3D_OK;
        }
        const HRESULT hr = o_CreateTexture(d, w, h, lv, use | D3DUSAGE_DYNAMIC, fmt, D3DPOOL_DEFAULT, pp, sh);
        EsAdd(g_esTexCreate, EsNow() - t0);
        if (SUCCEEDED(hr)) InterlockedIncrement(&g_exManaged[0]); else ExNoteFail("texture", hr);
        if (SUCCEEDED(hr) && pp && *pp && !o_TexLockRect) o_TexLockRect = (TexLockRect_t)HookSlot(*(void***)*pp, 19, (void*)&Hook_TexLockRect);
        return hr;
    }
    const HRESULT hr = o_CreateTexture(d, w, h, lv, use, fmt, pool, pp, sh);
    if (SUCCEEDED(hr) && pp && *pp && (use & D3DUSAGE_RENDERTARGET) && g_lensWantW && w == (UINT)g_lensWantW && h == (UINT)g_lensWantH) {
        InterlockedExchange(&g_lensWantW, 0);
        (*pp)->AddRef();
        IDirect3DTexture9* old = (IDirect3DTexture9*)InterlockedExchangePointer((void* volatile*)&g_lensTex, *pp);
        if (old) old->Release();
        Log("[OLVR][LENSCAM] the camcorder lens's render target on the GPU: %ux%u format %d", w, h, (int)fmt);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CreateVolumeTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT dp, UINT lv, DWORD use, D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9** pp, HANDLE* sh) {
    if (pool == D3DPOOL_MANAGED && g_d3d9Ex) {
        const HRESULT hr = o_CreateVolumeTexture(d, w, h, dp, lv, use | D3DUSAGE_DYNAMIC, fmt, D3DPOOL_DEFAULT, pp, sh);
        if (SUCCEEDED(hr)) InterlockedIncrement(&g_exManaged[0]); else ExNoteFail("volume texture", hr);
        return hr;
    }
    return o_CreateVolumeTexture(d, w, h, dp, lv, use, fmt, pool, pp, sh);
}
static HRESULT STDMETHODCALLTYPE Hook_CreateCubeTexture(IDirect3DDevice9* d, UINT e, UINT lv, DWORD use, D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9** pp, HANDLE* sh) {
    if (pool == D3DPOOL_MANAGED && g_d3d9Ex) {
        const HRESULT hr = o_CreateCubeTexture(d, e, lv, use | D3DUSAGE_DYNAMIC, fmt, D3DPOOL_DEFAULT, pp, sh);
        if (SUCCEEDED(hr)) InterlockedIncrement(&g_exManaged[0]); else ExNoteFail("cube texture", hr);
        return hr;
    }
    return o_CreateCubeTexture(d, e, lv, use, fmt, pool, pp, sh);
}
static HRESULT STDMETHODCALLTYPE Hook_CreateVertexBuffer(IDirect3DDevice9* d, UINT len, DWORD use, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** pp, HANDLE* sh) {
    if (pool == D3DPOOL_MANAGED && g_d3d9Ex) {
        const double t0 = EsNow();
        const HRESULT hr = o_CreateVertexBuffer(d, len, use, fvf, D3DPOOL_DEFAULT, pp, sh);
        EsAdd(g_esBufCreate, EsNow() - t0);
        if (SUCCEEDED(hr)) InterlockedIncrement(&g_exManaged[1]); else ExNoteFail("vertex buffer", hr);
        if (SUCCEEDED(hr) && pp && *pp && !o_VbLock) o_VbLock = (BufLock_t)HookSlot(*(void***)*pp, 11, (void*)&Hook_VbLock);
        return hr;
    }
    return o_CreateVertexBuffer(d, len, use, fvf, pool, pp, sh);
}
static HRESULT STDMETHODCALLTYPE Hook_CreateIndexBuffer(IDirect3DDevice9* d, UINT len, DWORD use, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9** pp, HANDLE* sh) {
    if (pool == D3DPOOL_MANAGED && g_d3d9Ex) {
        const double t0 = EsNow();
        const HRESULT hr = o_CreateIndexBuffer(d, len, use, fmt, D3DPOOL_DEFAULT, pp, sh);
        EsAdd(g_esBufCreate, EsNow() - t0);
        if (SUCCEEDED(hr)) InterlockedIncrement(&g_exManaged[1]); else ExNoteFail("index buffer", hr);
        if (SUCCEEDED(hr) && pp && *pp && !o_IbLock) o_IbLock = (BufLock_t)HookSlot(*(void***)*pp, 11, (void*)&Hook_IbLock);
        return hr;
    }
    return o_CreateIndexBuffer(d, len, use, fmt, pool, pp, sh);
}
extern "C" void OLProxy_ExStats(long* tex, long* buf, long* fail) {
    if (tex) *tex = g_exManaged[0]; if (buf) *buf = g_exManaged[1]; if (fail) *fail = g_exManaged[2];
}

static void InstallDeviceHooks(IDirect3DDevice9* dev) {
    if (InterlockedCompareExchange(&g_devHookDone, 1, 0) != 0) return;
    void** vt = *(void***)dev;
    g_dev = dev;
    if (g_d3d9Ex) {   // [D3D9EX] 23..27 = CreateTexture, CreateVolumeTexture, CreateCubeTexture, CreateVertexBuffer, CreateIndexBuffer
        o_CreateTexture       = (CreateTexture_t)      HookSlot(vt, 23, (void*)&Hook_CreateTexture);
        o_CreateVolumeTexture = (CreateVolumeTexture_t)HookSlot(vt, 24, (void*)&Hook_CreateVolumeTexture);
        o_CreateCubeTexture   = (CreateCubeTexture_t)  HookSlot(vt, 25, (void*)&Hook_CreateCubeTexture);
        o_CreateVertexBuffer  = (CreateVertexBuffer_t) HookSlot(vt, 26, (void*)&Hook_CreateVertexBuffer);
        o_CreateIndexBuffer   = (CreateIndexBuffer_t)  HookSlot(vt, 27, (void*)&Hook_CreateIndexBuffer);
        Log("[OLVR][D3D9EX] MANAGED-pool translation hooks installed (textures -> DEFAULT+DYNAMIC, buffers -> DEFAULT)");
    }
    o_Reset   = (Reset_t)  HookSlot(vt, 16, (void*)&Hook_Reset);
    o_Present = (Present_t)HookSlot(vt, 17, (void*)&Hook_Present);
    o_SetGammaRamp = (SetGammaRamp_t)HookSlot(vt, 21, (void*)&Hook_SetGammaRamp);   // [GAMMA]
    o_CreateQuery = (CreateQuery_t)HookSlot(vt, 118, (void*)&Hook_CreateQuery);
    // [UIMIRROR]: 57 = SetRenderState (state cache), 81..84 = the four draw entry points.
    o_SetRenderTarget        = (SetRenderTarget_t)       HookSlot(vt, 37, (void*)&Hook_SetRenderTarget);
    o_SetRenderState         = (SetRenderState_t)        HookSlot(vt, 57, (void*)&Hook_SetRenderState);
    o_DrawPrimitive          = (DrawPrimitive_t)         HookSlot(vt, 81, (void*)&Hook_DrawPrimitive);
    o_DrawIndexedPrimitive   = (DrawIndexedPrimitive_t)  HookSlot(vt, 82, (void*)&Hook_DrawIndexedPrimitive);
    o_DrawPrimitiveUP        = (DrawPrimitiveUP_t)       HookSlot(vt, 83, (void*)&Hook_DrawPrimitiveUP);
    o_DrawIndexedPrimitiveUP = (DrawIndexedPrimitiveUP_t)HookSlot(vt, 84, (void*)&Hook_DrawIndexedPrimitiveUP);
    // The camera census that used to hook SetVertexShaderConstantF (vtable 94) is NOT installed
    // any more. It was a research probe from the days of hunting the camera through the renderer,
    // the engine-side route won instead, and it never surfaced anything useful after that. It
    // was also the single most expensive thing in this dll: the game pushes about 14,000 vertex
    // constants per frame and every one of them was going through this hook to be
    // pattern-matched. Removing
    // it costs nothing and gives those cycles back.
    Log("[OLVR] device hooks installed dev=%p (CreateQuery 118, SetRenderState 57, draws 81-84)", (void*)dev);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocus,
                                                   DWORD Behavior, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** ppDev) {
    if (pp && g_xrAllowed && !g_keepVsync && pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        Log("[OLVR][PACING] CreateDevice PresentationInterval 0x%08lX -> IMMEDIATE", pp->PresentationInterval);
        pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    HRESULT hr;
    if (g_d3d9Ex && pp && ppDev) {
        // [D3D9EX] The Ex device, so resources can be shared with the headset bridge.
        D3DDISPLAYMODEEX fm = {}; fm.Size = sizeof(fm);
        fm.Width = pp->BackBufferWidth; fm.Height = pp->BackBufferHeight;
        fm.RefreshRate = pp->FullScreen_RefreshRateInHz; fm.Format = pp->BackBufferFormat;
        fm.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
        IDirect3DDevice9Ex* dx = nullptr;
        hr = ((IDirect3D9Ex*)This)->CreateDeviceEx(Adapter, DeviceType, hFocus, Behavior, pp,
                                                    pp->Windowed ? nullptr : &fm, &dx);
        *ppDev = SUCCEEDED(hr) ? dx : nullptr;
        Log("[OLVR][D3D9EX] CreateDeviceEx hr=0x%08lX dev=%p", hr, (void*)dx);
        if (FAILED(hr)) {
            // Never leave the game without a device: an ordinary one, MANAGED pool untouched.
            InterlockedExchange(&g_d3d9Ex, 0);
            hr = o_CreateDevice(This, Adapter, DeviceType, hFocus, Behavior, pp, ppDev);
            Log("[OLVR][D3D9EX] fell back to an ordinary device hr=0x%08lX (frames go through the readback path)", hr);
        }
    } else {
        hr = o_CreateDevice(This, Adapter, DeviceType, hFocus, Behavior, pp, ppDev);
    }
    if (hFocus) g_gameWnd = hFocus;
    if (pp && SUCCEEDED(hr)) {
        // [HIRES] A device created straight at the high-res size never sees a Reset, and the
        // render-scale nudge (HiResNudge) only runs for a windowed device.
        g_isWindowed = pp->Windowed ? 1 : 0;
        if (pp->hDeviceWindow) g_gameWnd = pp->hDeviceWindow;
    }
    if (pp) {
        Log("[OLVR] CreateDevice %ux%u bbFmt=%d windowed=%d behavior=0x%08lX hr=0x%08lX",
            pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->BackBufferFormat, (int)pp->Windowed, Behavior, hr);
        g_bbW = pp->BackBufferWidth; g_bbH = pp->BackBufferHeight;
        if (g_spoofArmed)
            Log("[OLVR][SCREQ] asked %ldx%ld, game took %ux%u (monitor queries spoofed=%ld)",
                g_reqW, g_reqH, g_bbW, g_bbH, g_dispqHits);
    }
    if (SUCCEEDED(hr) && ppDev && *ppDev) InstallDeviceHooks(*ppDev);
    if (SUCCEEDED(hr) && pp && pp->Windowed && g_spoofArmed &&
        ((LONG)pp->BackBufferWidth < g_reqW || (LONG)pp->BackBufferHeight < g_reqH)) {
        Log("[OLVR][SCREQ] initial windowed device is below request - arming coherent client-size resize");
        ArmWindowFix();
    }
    return hr;
}

static void InstallD3D9Hook(IDirect3D9* d3d) {
    if (InterlockedCompareExchange(&g_d3d9HookDone, 1, 0) != 0) return;
    void** vt = *(void***)d3d;
    o_CreateDevice = (CreateDevice_t)HookSlot(vt, 16, (void*)&Hook_CreateDevice);
    o_GetAdapterDisplayMode = (GetAdapterDisplayMode_t)HookSlot(vt, 8, (void*)&Hook_GetAdapterDisplayMode);
    Log("[OLVR] IDirect3D9 CreateDevice(16) + GetAdapterDisplayMode(8) hooks installed d3d9=%p", (void*)d3d);
}

// ---------------------------------------------------------------------------
// exports (the 4 names OLGame.exe imports)
// ---------------------------------------------------------------------------
typedef IDirect3D9* (WINAPI *Direct3DCreate9_t)(UINT);
static bool IniPath(wchar_t* out, size_t n);
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    EnsureRealD3D9();
    Direct3DCreate9_t real = g_realD3D9 ? (Direct3DCreate9_t)GetProcAddress(g_realD3D9, "Direct3DCreate9") : nullptr;
    if (!real) { Log("[OLVR] ERROR real Direct3DCreate9 missing"); return nullptr; }
    // [9ON12] (Test35) Windows' own D3D9-on-D3D12 layer. The game keeps classic D3D9 rules (managed
    // textures included), and every resource has a D3D12 resource underneath that can be handed
    // to the headset bridge without leaving the GPU ([ZEROCOPY], ol_xr.cpp). On the Quest 3 /
    // RTX 4050 laptop it took gameplay in the headset from 24 fps (native D3D9, where every frame
    // readback stalls the GPU) to ~56 with far fewer hitches. Native D3D9 is ~18% faster on its
    // own, so this only pays off with a headset. [XR] D3D9On12=0 goes back to native D3D9.
    {
        // [D3D9EX] (Test36) see the [D3D9EX] block above; preferred over D3D9On12 when on.
        wchar_t ini[MAX_PATH];
        if (IniPath(ini, MAX_PATH) && GetPrivateProfileIntW(L"XR", L"D3D9Ex", 1, ini) &&
            GetPrivateProfileIntW(L"XR", L"Enabled", 1, ini)) {
            g_texShadow = GetPrivateProfileIntW(L"XR", L"TextureShadow", 1, ini) ? 1 : 0;   // (Test47) [TEXSHADOW]
            typedef HRESULT (WINAPI *Create9Ex_t)(UINT, IDirect3D9Ex**);
            Create9Ex_t c = (Create9Ex_t)GetProcAddress(g_realD3D9, "Direct3DCreate9Ex");
            IDirect3D9Ex* ex = nullptr;
            const HRESULT hr = c ? c(SDKVersion, &ex) : E_NOINTERFACE;
            if (SUCCEEDED(hr) && ex) {
                InterlockedExchange(&g_d3d9Ex, 1);
                Log("[OLVR][D3D9EX] Direct3DCreate9 sdk=%u -> Direct3DCreate9Ex %p", SDKVersion, (void*)ex);
                InstallD3D9Hook(ex);
                return ex;
            }
            Log("[OLVR][D3D9EX] Direct3DCreate9Ex unavailable (hr=0x%08lX)", hr);
        }
    }
    {
        wchar_t ini[MAX_PATH];
        if (IniPath(ini, MAX_PATH) && GetPrivateProfileIntW(L"XR", L"D3D9On12", 1, ini) &&
            GetPrivateProfileIntW(L"XR", L"Enabled", 1, ini)) {
            typedef IDirect3D9* (WINAPI *Create9On12_t)(UINT, D3D9ON12_ARGS*, UINT);
            Create9On12_t c = (Create9On12_t)GetProcAddress(g_realD3D9, "Direct3DCreate9On12");
            D3D9ON12_ARGS args = {}; args.Enable9On12 = TRUE;
            IDirect3D9* d = c ? c(SDKVersion, &args, 1) : nullptr;
            if (d) {
                Log("[OLVR][9ON12] Direct3DCreate9 sdk=%u -> D3D9On12 %p", SDKVersion, (void*)d);
                InstallD3D9Hook(d);
                return d;
            }
            Log("[OLVR][9ON12] Direct3DCreate9On12 unavailable - plain D3D9");
        }
    }
    IDirect3D9* d3d = real(SDKVersion);
    Log("[OLVR] Direct3DCreate9 sdk=%u real=%p", SDKVersion, (void*)d3d);
    if (d3d) InstallD3D9Hook(d3d);
    return d3d;
}
typedef int  (WINAPI *D3DPERF_BeginEvent_t)(D3DCOLOR, LPCWSTR);
typedef int  (WINAPI *D3DPERF_EndEvent_t)(void);
typedef void (WINAPI *D3DPERF_SetOptions_t)(DWORD);
extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name) { EnsureRealD3D9();
    static D3DPERF_BeginEvent_t r = g_realD3D9 ? (D3DPERF_BeginEvent_t)GetProcAddress(g_realD3D9, "D3DPERF_BeginEvent") : nullptr; return r ? r(col, name) : 0; }
extern "C" int WINAPI D3DPERF_EndEvent(void) { EnsureRealD3D9();
    static D3DPERF_EndEvent_t r = g_realD3D9 ? (D3DPERF_EndEvent_t)GetProcAddress(g_realD3D9, "D3DPERF_EndEvent") : nullptr; return r ? r() : 0; }
extern "C" void WINAPI D3DPERF_SetOptions(DWORD opt) { EnsureRealD3D9();
    static D3DPERF_SetOptions_t r = g_realD3D9 ? (D3DPERF_SetOptions_t)GetProcAddress(g_realD3D9, "D3DPERF_SetOptions") : nullptr; if (r) r(opt); }

// ---------------------------------------------------------------------------
// key poller + boot
// The MENU keys are bare, unlike every probe hotkey, and that is a deliberate exception. INSERT
// is not bound by Outlast, and the arrows only do anything while the menu is open - at which
// point you are standing still reading it. Nothing else is bound at all, because a key
// the game also handles makes its own test unreadable (two runs were lost learning that).
static bool Bare(int vk, bool& prev) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool fired = down && !prev;
    prev = down;
    return fired;
}

// ---- [RECENTER] one key, and a controller shortcut, both rebindable ---------------------------
// Recentering is the one thing a player needs mid-game without opening a menu: sit differently,
// drift, press it, carry on. So it gets a bare key of its own.
//
// The default is G, chosen after reading Outlast's OWN bindings out of OLInput.ini and
// DefaultInput.ini rather than guessing: W A S D, Q, E, F, R, J, N, Tab, Space, Escape, Ctrl,
// Shift, the arrows and the mouse are all taken. G is free and sits beside F (night vision),
// so the hand is already there. R was the first choice and was WRONG: R is OLA_Reload, the
// camcorder battery. The keypress is never swallowed, so any key the game also uses would fire
// both actions, and burning a battery to recentre is not a trade worth making.
//
// The controller shortcut is a DOUBLE press of R3, and both halves of that were chosen against
// the game's real bindings rather than by taste.
// DOUBLE, because a single press is too easy to hit by accident and having your view yanked
// straight while something is chasing you is the worst possible moment for it.
// R3, because every pad button in this game is bound to something and R3 is the safest of them:
//   * it is OLA_ToggleNightVision, a TOGGLE, so two presses land back in the state you started
//     in, and with the camcorder down it does nothing at all
//   * nobody presses it twice quickly in normal play
// L1 was tried first and is WORSE despite looking harmless: it is OLA_Run, the button players
// mash and re-grip during every chase, which is precisely the accidental double tap this design
// is trying to avoid. L3 is bound to ToggleDebugCamera and was rejected outright - if that is
// live in the shipping build it detaches the camera, which is far worse than a night-vision
// blink. Both bindings are rebindable from the menu and saved to the ini.
static volatile LONG g_recenterVk  = 'G';                 // 0 = no key bound
static volatile LONG g_recenterPad = 0x0100;              // XINPUT_GAMEPAD_LEFT_SHOULDER (L1)
// The menu key itself is rebindable too. INSERT is a fine default (Outlast does not use it) but
// it is missing or awkward on plenty of compact and laptop keyboards, and a settings menu you
// cannot open is the one binding with no way to fix itself from inside the game.
static volatile LONG g_menuVk      = VK_INSERT;
static volatile LONG g_bindCapture = 0;                   // 0 idle, 1 key, 2 pad, 3 menu key
static const DWORD kPadDoubleMs = 400;

extern "C" int  OLProxy_GetRecenterKey() { return (int)g_recenterVk; }
extern "C" int  OLProxy_GetRecenterPad() { return (int)g_recenterPad; }
extern "C" int  OLProxy_GetMenuKey()     { return (int)g_menuVk; }
extern "C" int  OLProxy_GetBindCapture() { return (int)g_bindCapture; }
extern "C" void OLProxy_StartBindCapture(int which) { InterlockedExchange(&g_bindCapture, which); }

// Human names, because "VK 0x52" is not something to show a player.
extern "C" const char* OLProxy_KeyName(int vk) {
    static char buf[64];
    if (vk == 0) { strcpy_s(buf, "none"); return buf; }
    if (vk >= 'A' && vk <= 'Z') { buf[0] = (char)vk; buf[1] = 0; return buf; }
    if (vk >= '0' && vk <= '9') { buf[0] = (char)vk; buf[1] = 0; return buf; }
    switch (vk) {
        case VK_INSERT: strcpy_s(buf, "Insert");    return buf;   // the menu default: name it
        case VK_SPACE:  strcpy_s(buf, "Space");     return buf;
        case VK_TAB:    strcpy_s(buf, "Tab");       return buf;
        case VK_BACK:   strcpy_s(buf, "Backspace"); return buf;
        case VK_HOME:   strcpy_s(buf, "Home");      return buf;
        case VK_END:    strcpy_s(buf, "End");       return buf;
        case VK_DELETE: strcpy_s(buf, "Delete");    return buf;
        case VK_PRIOR:  strcpy_s(buf, "Page Up");   return buf;
        case VK_NEXT:   strcpy_s(buf, "Page Down"); return buf;
        case VK_OEM_3:  strcpy_s(buf, "`");         return buf;
        case VK_OEM_MINUS:  strcpy_s(buf, "-");     return buf;
        case VK_OEM_PLUS:   strcpy_s(buf, "=");     return buf;
        case VK_OEM_4:  strcpy_s(buf, "[");         return buf;
        case VK_OEM_6:  strcpy_s(buf, "]");         return buf;
        case VK_OEM_1:  strcpy_s(buf, ";");         return buf;
        case VK_OEM_7:  strcpy_s(buf, "'");         return buf;
        case VK_OEM_COMMA:  strcpy_s(buf, ",");     return buf;
        case VK_OEM_PERIOD: strcpy_s(buf, ".");     return buf;
        case VK_OEM_2:  strcpy_s(buf, "/");         return buf;
        case VK_OEM_5:  strcpy_s(buf, "\\");        return buf;
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) { sprintf_s(buf, "F%d", vk - VK_F1 + 1); return buf; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { sprintf_s(buf, "Numpad %d", vk - VK_NUMPAD0); return buf; }
    sprintf_s(buf, "key %d", vk);
    return buf;
}

extern "C" const char* OLProxy_PadName(int mask) {
    switch (mask) {
        case 0x0020: return "Back / Select";
        case 0x0010: return "Start";
        case 0x0040: return "L3 (left stick click)";
        case 0x0080: return "R3 (right stick click)";
        case 0x0100: return "L1 (left bumper)";
        case 0x0200: return "R1 (right bumper)";
        case 0x1000: return "A";
        case 0x2000: return "B";
        case 0x4000: return "X";
        case 0x8000: return "Y";
        case 0x0001: return "D-pad up";
        case 0x0002: return "D-pad down";
        case 0x0004: return "D-pad left";
        case 0x0008: return "D-pad right";
        case 0:      return "none";
        default:     return "button";
    }
}

// XInput, loaded the same lazy way the menu does it. Absent controller = every read is zero,
// so the whole controller path simply never fires.
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, void*);
static XInputGetState_t g_padGet = nullptr;
static bool g_padTried = false;

static WORD PadButtons() {
    if (!g_padTried) {
        g_padTried = true;
        const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
        for (int i = 0; i < 3 && !g_padGet; ++i) {
            HMODULE m = LoadLibraryW(dlls[i]);
            if (m) g_padGet = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
        }
        Log("[OLVR][RECENTER] controller support %s", g_padGet ? "ready" : "unavailable (no XInput)");
    }
    if (!g_padGet) return 0;
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st = {};
    if (g_padGet(0, &st) != ERROR_SUCCESS) return 0;
    return st.buttons;
}

// ---- [PADSTUB] motion controls, step 1: can a FABRICATED pad drive this game? ------------------
// The whole motion-control plan rests on one unproven assumption - that this hook can BE the controller,
// not merely add to one. The lean build only ever merged into a pad that was already plugged in
// (and it worked, so the hook and the game's reading of merged values are already proven). The
// open question is the NO-PAD case: UE3's WinDrv polls XInputGetState per tick, and some builds
// stop polling a slot that answered ERROR_DEVICE_NOT_CONNECTED at startup. If this game does
// that, no synthetic pad can ever reach it and the OpenXR action set would be a week spent on a
// dead end. One afternoon here answers it.
//
// Two measurements, both from this one hook:
//   1. DOES IT EVEN ASK? the poll counters below, reported every ~3 s from KeyThread (an
//      independent timer - counting from inside the hook could not distinguish "polling stopped"
//      from "never started"). Slot 0 and all-slots are counted separately, so "polls other slots
//      but not 0" is distinguishable from silence.
//   2. DOES IT OBEY? with [Stage3] PadStub=1 and no physical pad, answer ERROR_SUCCESS with a
//      stick that walks forward 1.5 s then back 1.5 s, forever. Miles pacing on his own with
//      nothing plugged in is the proof.
// The stick OSCILLATES rather than holding forward, deliberately: a constant walk is
// indistinguishable from a stuck analog stick or a slope, and it would walk him off a ledge while
// you watch. Alternating is unmistakably this hook's doing and returns Miles to roughly where he started.
// A real connected pad ALWAYS wins - the stub only fills a slot the runtime says is empty - so
// this can never fight a controller you are holding.
struct OLXiGamepad { WORD wButtons; BYTE bLeftTrigger; BYTE bRightTrigger;
                     SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct OLXiState   { DWORD dwPacketNumber; OLXiGamepad Gamepad; };
typedef DWORD (WINAPI *XiGetState_t)(DWORD, OLXiState*);
static XiGetState_t  o_XiGetState = nullptr;
static volatile long g_padStub     = 0;   // [Stage3] PadStub, dev only, default off
static volatile long g_padPolls0   = 0;   // slot-0 polls since the last report
static volatile long g_padPollsAll = 0;   // polls of ANY slot since the last report
static volatile long g_padRealOk   = 0;   // did the real device answer ERROR_SUCCESS last time
static volatile long g_padFaked    = 0;   // frames answered for an empty slot

// [XRINPUT] THE MAP. One table, one place - the whole point of publishing a fabricated bitmask from
// ol_xr.cpp instead of XInput's. Chosen against Outlast's own bindings (OLInput.ini):
//   A = OLA_Jump      B = OLA_CrouchToggle   X = OLA_Use        Y = OLA_Reload (camcorder battery)
//   LB = OLA_Run      RB = OLA_ToggleCamcorder                  R3 = OLA_ToggleNightVision
// Legacy Touch layout (MouseUse=0): A/B jump/crouch, X/Y use/battery. MouseUse=1 swaps
// gameplay X/B and sends B through the mouse. Grips are run (the natural
// "hold to sprint"), right stick click is night vision, and the triggers keep the game's own
// analog lean. Menu opens the pause menu (Start).
// The game's camcorder command is a toggle, so the VR control must also be a toggle.  The old
// implementation sent one toggle when the grip closed and another when it opened; a normal
// squeeze therefore raised and immediately lowered the camera, and the zoom gate was almost
// impossible to keep open.  Keep our last requested state and send exactly one RB pulse for
// each squeeze instead.
static bool  g_camcorderUp = false;
static bool  g_camGripDown = false;
static volatile long g_menuChordLatch = 0;   // [TOUCHMENU] Menu held since the chord fired
static bool  g_nvRequested = false;
extern "C" int OLProxy_NightVisionActive() { return (g_camcorderUp && g_nvRequested) ? 1 : 0; }
// (Test43) [HANDNV] ol_interact keeps this in step with the game's own CamcorderMode at your eye
extern "C" void OLProxy_SetNvRequested(int on) { g_nvRequested = on != 0; }

// [MOUSEUSE] The requested gameplay layout: B = left mouse, X = crouch.
// Menu and left-grip bindings retain the physical button names. 0 restores
// the complete legacy layout. Read once before the XR worker starts.
static volatile LONG g_mouseUse = 1;
extern "C" int OLProxy_MouseUseEnabled() { return g_mouseUse ? 1 : 0; }
extern "C" int OLUE3_InGameMenu();
extern "C" int OLUE3_GameplayActive();
static SRWLOCK g_mouseUseLock = SRWLOCK_INIT;
static OLMouseUse g_mouseUseState;
static ULONGLONG g_mouseUseSampleMs = 0;
static volatile LONG g_mouseUseStopping = 0, g_mouseUseHeld = 0;

static bool SendUseMouseEvent(bool down) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    input.mi.dwExtraInfo = OLMouseUse::InputTag;
    // Publish ownership before entering Windows: process teardown can terminate
    // the worker inside SendInput before its return value is processed.
    if (down) InterlockedExchange(&g_mouseUseHeld, 1);
    const bool ok = SendInput(1, &input, sizeof(input)) == 1;
    if ((down && !ok) || (!down && ok)) InterlockedExchange(&g_mouseUseHeld, 0);
    return ok;
}
static bool SendUseMouse(bool down) {
    const bool ok = SendUseMouseEvent(down);
    static unsigned failures = 0;
    if (ok || failures++ < 5)
        Log("[OLVR][MOUSEUSE] left mouse %s: %s", down ? "down" : "up", ok ? "sent" : "FAILED");
    if (ok) failures = 0;
    return ok;
}
static bool MouseUseContext() {
    return g_mouseUse && !g_mouseUseStopping && g_gameWnd &&
           GetForegroundWindow() == g_gameWnd && !OLMenu_Visible() && !OLUE3_InGameMenu();
}
// Called on every RAW XR sample, before publication. Even a whole tap
// between game polls reaches Windows as down/up; there is no 100 ms mouse tail.
extern "C" void OLProxy_MouseUseSample(unsigned buttons, float gripL, int focused) {
    AcquireSRWLockExclusive(&g_mouseUseLock);
    g_mouseUseSampleMs = GetTickCount64();
    const bool allowed = focused && MouseUseContext() && gripL <= 0.6f;
    const bool canStart = OLUE3_GameplayActive() && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000);
    if (!focused) g_mouseUseState.Cancel(SendUseMouse);
    else g_mouseUseState.Sample((buttons & OLXR_BTN_B) != 0, allowed, canStart, SendUseMouse);
    if (g_mouseUseStopping) g_mouseUseState.Cancel(SendUseMouse);
    ReleaseSRWLockExclusive(&g_mouseUseLock);
}
// Independent of XInput polling and xrWaitFrame, including while binding keys.
static void MouseUseWatchdog() {
    AcquireSRWLockExclusive(&g_mouseUseLock);
    if (!MouseUseContext() || g_mouseUseState.releasePending || GetTickCount64() - g_mouseUseSampleMs > 200)
        g_mouseUseState.Cancel(SendUseMouse);
    ReleaseSRWLockExclusive(&g_mouseUseLock);
}
extern "C" void OLProxy_MouseUseShutdown() {
    InterlockedExchange(&g_mouseUseStopping, 1);
    // DllMain holds the loader lock: never wait for the worker here.
    if (TryAcquireSRWLockExclusive(&g_mouseUseLock)) {
        g_mouseUseState.Cancel(SendUseMouseEvent);
        ReleaseSRWLockExclusive(&g_mouseUseLock);
    } else if (InterlockedExchange(&g_mouseUseHeld, 0)) {
        SendUseMouseEvent(false);
    }
}

// OpenXR and UE3/XInput are sampled on different clocks. A very quick Touch-button tap can be
// present for one XR sample yet fall wholly between two game polls. Extend discrete presses for
// 100 ms. Continuous controls (sprint and zoom) deliberately bypass this helper so release is
// immediate.
static WORD ReliablePadButtons(WORD raw, bool resetFace = false) {
    static const WORD bits[] = { 0x0004, 0x0008, 0x0010, 0x0020, 0x0080,
                                 0x1000, 0x2000, 0x4000, 0x8000 };
    // 64-bit: a 32-bit "(LONG)(until - now) > 0" against the initial 0 is TRUE whenever the PC's
    // uptime is between 24.9 and 49.7 days, which held every one of these buttons forever.
    static ULONGLONG until[_countof(bits)] = {};
    const ULONGLONG now = GetTickCount64();
    WORD out = raw;
    for (unsigned i = 0; i < _countof(bits); ++i) {
        if (resetFace && (bits[i] == 0x2000 || bits[i] == 0x4000)) until[i] = 0;
        if (raw & bits[i]) until[i] = now + 100;
        else if (now < until[i]) out |= bits[i];
    }
    return out;
}

static OLFaceButtonGate g_faceGate;
static WORD MapPadButtons(const OLXrPad& p, bool inMenu = false) {
    const bool resetFace = OLProxy_MouseUseEnabled() && g_faceGate.inMenu != inMenu;
    const bool mouseLayout = OLProxy_MouseUseEnabled() && !inMenu;
    const unsigned buttons = OLProxy_MouseUseEnabled() ? g_faceGate.Apply(p.buttons, inMenu) : p.buttons;
    WORD b = 0;
    // (Beta 1.1.4) [FACECAM] R3 is both the night-vision switch (a click) and the camcorder's
    // raise / lower at the face (hold it). The game's night-vision press is withheld until the
    // click is released, and only a short click sends it: a held click must not flip night vision
    // while it raises or lowers the camcorder. ol_interact.cpp measures the same hold.
    static bool nvDown = false;
    static ULONGLONG nvDownAt = 0, nvPulseUntil = 0;
    const bool nvNow = (buttons & OLXR_BTN_RSTICK) != 0;
    const ULONGLONG nowMs = GetTickCount64();
    if (nvNow && !nvDown) { nvDown = true; nvDownAt = nowMs; }
    else if (!nvNow && nvDown) {
        nvDown = false;
        if (nowMs - nvDownAt < OLVR_CAM_HOLD_MS) {
            nvPulseUntil = nowMs + 120;
            if (g_camcorderUp) g_nvRequested = !g_nvRequested;
        }
    }
    if (nowMs < nvPulseUntil) b |= 0x0080;              // R3 = OLA_ToggleNightVision (short click)
    const bool shift = p.gripL > 0.6f;
    // (Test48) A is not a left-grip chord: nothing is on left grip + A, and the left grip is also sprint, so
    // counting it dropped BOTH the sprint and the jump the moment A was pressed while running - "most of the time
    // running and vaulting with A does not work".
    // (Beta 1.1.4) left grip + B and left grip + X are gone: they opened the notebook / recordings
    // (the documents menu) by accident while sprinting. Only Y still has a grip chord. B and X stay
    // silent while the grip is held, so the sprint cannot also crouch, use or open a menu.
    const bool chord = shift && (buttons & OLXR_BTN_Y);
    if (shift && (buttons & OLXR_BTN_Y)) b |= 0x0008;   // left grip + Y = evidence/documents
    if (!chord) {
        if (buttons & OLXR_BTN_A) b |= 0x1000;           // A  = OLA_Jump
        if (!shift) {
            if (mouseLayout) {
                if (buttons & OLXR_BTN_X) b |= 0x2000;   // physical X = crouch
                // Physical B is delivered as RAW mouse down/up by the XR sampler.
            } else {
                if (buttons & OLXR_BTN_B) b |= 0x2000;   // menus/legacy: B
                if (buttons & OLXR_BTN_X) b |= 0x4000;   // menus/legacy: X
            }
        }
        if (buttons & OLXR_BTN_Y) b |= 0x8000;           // Y  = OLA_Reload (camcorder battery)
    }
    // START = pause menu. Left grip + Menu is the mod menu instead ([TOUCHMENU]). (Test39) Start is
    // sent when Menu is RELEASED, and only if the left grip never joined the press: squeezing the
    // grip a moment after Menu used to open the game's pause menu AND the mod menu together.
    {
        static bool s_menuHeld = false, s_menuChorded = false;
        static ULONGLONG s_startUntil = 0;
        if (buttons & OLXR_BTN_MENU) {
            if (!s_menuHeld) { s_menuHeld = true; s_menuChorded = false; }
            if (shift || g_menuChordLatch || p.gripL > 0.35f) s_menuChorded = true;
        } else if (s_menuHeld) {
            s_menuHeld = false;
            if (!s_menuChorded) s_startUntil = GetTickCount64() + 120;
        }
        if (GetTickCount64() < s_startUntil) b |= 0x0010;
    }
    if (!chord && (shift || (buttons & OLXR_BTN_LSTICK))) b |= 0x0100; // grip or L3 = sprint
    b = ReliablePadButtons(b, resetFace);
    // camera up: right stick up = zoom in, down = zoom out. (Test46) A turn is never a perfectly
    // level push, so the zoom needs a mostly-vertical stick - but the first rule (|y| > 0.60 AND
    // |y| > 2|x|) was so tight that an ordinary push never engaged and the zoom looked dead (the
    // W-T bar never moved). Half deflection and "more vertical than horizontal" is still a
    // deliberate push and still cannot fire while turning, where |x| dominates.
    static int s_zoom = 0;
    if (g_camcorderUp) {
        const float ax = fabsf(p.lookX), ay = fabsf(p.lookY);
        if (s_zoom == 0) { if (ay > 0.45f && ay > ax) s_zoom = p.lookY > 0.0f ? 1 : -1; }
        else if (ay < 0.25f || ax > ay) s_zoom = 0;
    } else s_zoom = 0;
    if (s_zoom > 0) b |= 0x0001;
    else if (s_zoom < 0) b |= 0x0002;
    // RIGHT GRIP is handled by the caller, not here: it is a HOLD that has to send the game's
    // toggle exactly once on press and once on release ([HANDCAM]), which needs edge state.
    return b;
}

// [MENUPRESS] (Test39) In the game's menus every press reaches the game as one short tap (70 ms),
// however long the button is held. Outlast's note view closes the whole notebook when the A that
// opened it is still down as it appears (a raw pad press held 300 ms did it; 80 ms did not), and a
// Quest press plus the 100 ms hold-over above is often that long - "I click on the note and it
// closes to gameplay". Grips become LB / RB taps there (the notebook's tab switches). Leaving a menu,
// whatever is still held from inside it stays silent until released, so the A that picked "Resume"
// cannot also jump.
extern "C" int OLUE3_InGameMenu();
static WORD MenuTaps(WORD held, bool inMenu) {
    static WORD s_prev = 0, s_fromMenu = 0;
    static ULONGLONG s_until[16] = {};
    const ULONGLONG now = GetTickCount64();
    if (!inMenu) {
        s_fromMenu &= held;                       // released bits are free again
        s_prev = held;
        return (WORD)(held & ~s_fromMenu);
    }
    const WORD rising = (WORD)(held & ~s_prev);
    s_prev = held; s_fromMenu = held;
    WORD out = 0;
    for (int i = 0; i < 16; ++i) {
        const WORD bit = (WORD)(1u << i);
        if (rising & bit) s_until[i] = now + 70;
        if (now < s_until[i]) out |= bit;
    }
    return out;
}

// [HANDCAM] Right grip = toggle the camcorder. The game already treats RB as a toggle, so one
// squeeze produces one RB pulse; release is silent. Hysteresis prevents a grip resting around
// the threshold from producing multiple squeezes.
static DWORD g_gripEdgeUntil = 0;   // keep RB pressed briefly so the game cannot miss the edge
extern "C" void OLXR_SetHandCamActive(int on);
static WORD CamcorderGripButton(const OLXrPad& p) {
    bool down = g_camGripDown;
    if (!down && p.gripR > 0.65f) down = true;
    else if (down && p.gripR < 0.45f) down = false;
    if (down && !g_camGripDown) {
        g_camcorderUp = !g_camcorderUp;
        // Hold the pulse long enough to cross at least several UE3 polls.
        g_gripEdgeUntil = GetTickCount() + 120;
        OLXR_SetHandCamActive(g_camcorderUp && !OLInteract_CamButton() ? 1 : 0);   // (Test49) grip button: the head aims
    }
    g_camGripDown = down;
    return (GetTickCount() < g_gripEdgeUntil) ? 0x0200 : 0;   // RIGHT_SHOULDER
}
static SHORT StickAxis(float v) {
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    const float s = v * 32767.0f;
    return (SHORT)(s > 32767.0f ? 32767 : (s < -32768.0f ? -32768 : s));
}

static DWORD WINAPI Hook_XiGetState(DWORD idx, OLXiState* st) {
    InterlockedIncrement(&g_padPollsAll);
    const DWORD r = o_XiGetState ? o_XiGetState(idx, st) : ERROR_DEVICE_NOT_CONNECTED;
    if (idx != 0 || !st) return r;
    InterlockedIncrement(&g_padPolls0);
    InterlockedExchange(&g_padRealOk, r == ERROR_SUCCESS ? 1 : 0);

    // [XRINPUT] VR controllers become the pad. Merged ADDITIVELY on top of whatever the real
    // device reported, so a physical pad, the keyboard and the controllers all work at once and
    // nobody's existing setup breaks - the release rule for this mod.
    // [DEVBRIDGE] A scripted test press reaches the game whatever else is connected; with the
    // bridge on and no controllers, slot 0 always reads as a connected pad so presses land.
    {
        WORD db = 0; float dlx = 0, dly = 0, drx = 0, dry = 0;
        const int dev = OLDev_Pad(&db, &dlx, &dly, &drx, &dry);
        if (dev == 2 || (dev == 1 && !OLXR_MotionControlsReady())) {
            static DWORD s_devPacket = 0;
            if (r != ERROR_SUCCESS) ZeroMemory(st, sizeof(*st));
            st->dwPacketNumber = ++s_devPacket;
            st->Gamepad.wButtons |= db;
            if (dev == 2 && (dlx || dly)) { OLInteract_FilterMoveStick(&dlx, &dly); st->Gamepad.sThumbLX = StickAxis(dlx); st->Gamepad.sThumbLY = StickAxis(dly); }
            if (dev == 2 && (drx || dry)) { st->Gamepad.sThumbRX = StickAxis(drx); st->Gamepad.sThumbRY = StickAxis(dry); }
            return ERROR_SUCCESS;
        }
    }
    OLXrPad p;
    if (OLXR_MotionControlsReady() && OLMenu_Visible()) {
        // [TOUCHMENU] The sticks and buttons are navigating the mod menu. Hand the game a neutral
        // connected pad (a real pad's own state passes through untouched) so nothing walks,
        // jumps or pauses underneath it, and the slot never reads as unplugged.
        static DWORD s_menuPacket = 0;
        if (r != ERROR_SUCCESS) { ZeroMemory(st, sizeof(*st)); st->dwPacketNumber = ++s_menuPacket; }
        if (OLProxy_MouseUseEnabled() && OLXR_GetPad(&p)) g_faceGate.Apply(p.buttons, true);
        return ERROR_SUCCESS;
    }
    if (OLXR_MotionControlsReady() && OLXR_GetPad(&p)) {
        static DWORD s_packet = 0;
        if (r != ERROR_SUCCESS) { ZeroMemory(st, sizeof(*st)); }   // empty slot: this hook IS the pad
        // [INTERACT] (Test37) The game's real camcorder state, grips that are busy grabbing a door
        // knob or a battery (they must not also sprint or toggle the camcorder), and the presses
        // the gestures ask for (raise/lower = RB, battery inserted = Y).
        OLInteractPad ip = {}; OLInteract_Pad(p, &ip);
        if (ip.camUp >= 0) g_camcorderUp = ip.camUp != 0;
        OLXrPad pg = p;
        if (ip.blockGripL) pg.gripL = 0.0f;
        if (ip.blockGripR) pg.gripR = 0.0f;
        const bool inMenu = OLUE3_InGameMenu() != 0;
        const WORD camEdge = inMenu ? 0 : CamcorderGripButton(pg);   // update camera state before mapping zoom
        // [PHASE1] Snap/smooth turn, walk direction, stick pitch and physical crouch rewrite the
        // sticks and may add a crouch press (ol_motion.cpp). Outside gameplay they pass through.
        float mx = p.moveX, my = p.moveY, lx = p.lookX;
        float ly = g_camcorderUp ? 0.0f : p.lookY;          // vertical stick owns zoom while camera is up
        const WORD motionButtons = OLMotion_Process(p, g_camcorderUp, &mx, &my, &lx, &ly);
        WORD btn = MapPadButtons(pg, inMenu) | camEdge | motionButtons | ip.addButtons;
        if (inMenu && pg.gripR > 0.65f) btn |= 0x0200;              // right grip = RB (a tab switch)
        st->Gamepad.wButtons |= MenuTaps(btn, inMenu);
        // Sticks: take the fabricated values only when actually deflected, so a real stick is never
        // fought. The walk gate reads the player's own stick; the rotated vector then goes in whole.
        if (fabsf(p.moveX) > 0.12f || fabsf(p.moveY) > 0.12f) {   // gate on the player's input
            OLInteract_FilterMoveStick(&mx, &my);                  // (Test41) not onto a ladder no hand holds
            st->Gamepad.sThumbLX = StickAxis(mx);
            st->Gamepad.sThumbLY = StickAxis(my);
        } else if (ip.stickOn && !inMenu) {
            // (Test40) [CLIMB] on a ladder your hand pulled the rung down: Miles climbs after it; a pull
            // on an edge steps him into it
            st->Gamepad.sThumbLX = StickAxis(ip.stickLX);
            st->Gamepad.sThumbLY = StickAxis(ip.stickLY);
        }
        if (fabsf(lx) > 0.12f || fabsf(ly) > 0.12f) {
            st->Gamepad.sThumbRX = StickAxis(lx);
            st->Gamepad.sThumbRY = StickAxis(ly);
        }
        const BYTE lt = (BYTE)(p.trigL * 255.0f), rt = (BYTE)(p.trigR * 255.0f);
        if (lt > st->Gamepad.bLeftTrigger)  st->Gamepad.bLeftTrigger  = lt;
        if (rt > st->Gamepad.bRightTrigger) st->Gamepad.bRightTrigger = rt;
        st->dwPacketNumber = ++s_packet;   // must change or a deduping reader skips this update
        InterlockedIncrement(&g_padFaked);
        return ERROR_SUCCESS;
    }

    if (!g_padStub || r == ERROR_SUCCESS) return r;   // a real pad always wins
    static DWORD s_stubPacket = 0;
    const DWORD phase = (GetTickCount() / 1500u) & 1u;
    ZeroMemory(st, sizeof(*st));
    // The packet number MUST change or an input layer that dedupes on it will skip the read -
    // which would look exactly like the game ignoring the fabricated pad and give a false negative.
    st->dwPacketNumber = ++s_stubPacket;
    st->Gamepad.sThumbLY = phase ? (SHORT)-24000 : (SHORT)24000;   // back / forward, well past deadzone
    InterlockedIncrement(&g_padFaked);
    return ERROR_SUCCESS;
}

// [HAPTICS] XInputSetState (xinput1_3 ordinal 3): the game's rumble. Always passed to the real
// device, so a physical pad still shakes; slot 0's levels also go to the VR controllers. While
// the controllers ARE the pad, slot 0 answers success like the GetState hook does, so the game
// never decides its pad vanished.
struct OLXiVibration { WORD wLeftMotorSpeed; WORD wRightMotorSpeed; };
typedef DWORD (WINAPI *XiSetState_t)(DWORD, OLXiVibration*);
static XiSetState_t o_XiSetState = nullptr;
static DWORD WINAPI Hook_XiSetState(DWORD idx, OLXiVibration* v) {
    const DWORD r = o_XiSetState ? o_XiSetState(idx, v) : ERROR_DEVICE_NOT_CONNECTED;
    if (idx != 0 || !v) return r;
    OLXR_SetRumble(v->wLeftMotorSpeed / 65535.0f, v->wRightMotorSpeed / 65535.0f);
    return OLXR_MotionControlsReady() ? ERROR_SUCCESS : r;
}

// Watch every key for the first fresh press and take it as the new binding. Modifiers and the
// menu's own navigation keys are skipped so a rebind cannot leave the menu unusable.
// forMenu picks which binding is being set, and it also decides whether INSERT is allowed: it
// has to be, or someone who rebinds the menu away from INSERT could never choose it again.
static void CaptureKeyBind(bool forMenu) {
    static const int kSkip[] = { VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN,
                                 VK_ESCAPE, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_RETURN };
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if ((GetAsyncKeyState(vk) & 0x8000) == 0) continue;
        if (!forMenu && vk == VK_INSERT) continue;
        bool skip = false;
        for (int i = 0; i < (int)(sizeof(kSkip) / sizeof(kSkip[0])); ++i) if (kSkip[i] == vk) skip = true;
        if (skip) continue;
        if (forMenu) {
            InterlockedExchange(&g_menuVk, vk);
            // Both on one key would open the menu and recenter at the same time. The menu wins,
            // and the recenter key steps aside rather than firing invisibly underneath it.
            if (g_recenterVk == vk) InterlockedExchange(&g_recenterVk, 0);
            Log("[OLVR][MENU] menu key rebound to %s", OLProxy_KeyName(vk));
        } else {
            InterlockedExchange(&g_recenterVk, vk);
            if (g_menuVk == vk) InterlockedExchange(&g_menuVk, VK_INSERT);
            Log("[OLVR][RECENTER] key rebound to %s", OLProxy_KeyName(vk));
        }
        InterlockedExchange(&g_bindCapture, 0);
        return;
    }
}

static void CapturePadBind() {
    const WORD b = PadButtons();
    if (!b) return;
    for (int bit = 0; bit < 16; ++bit) {
        const WORD mask = (WORD)(1 << bit);
        if (!(b & mask)) continue;
        InterlockedExchange(&g_recenterPad, mask);
        InterlockedExchange(&g_bindCapture, 0);
        Log("[OLVR][RECENTER] controller shortcut rebound to %s", OLProxy_PadName(mask));
        return;
    }
}

static DWORD WINAPI KeyThread(LPVOID) {
    bool pins = false, pup = false, pdn = false, plf = false, prt = false;
    bool pw = false, ps = false, pa = false, pd = false, pen = false, psp = false;
    bool prec = false, ppad = false;
    DWORD lastPadTapMs = 0;
    int  padTick = 0; bool padReported = false;   // [PADSTUB] report cadence
    for (;;) {
        MouseUseWatchdog();
        // [RECENTER] rebind capture first: while it is armed, the pressed key is a binding, not
        // a command, so nothing else in this loop should act on it.
        const LONG cap = g_bindCapture;
        if (cap == 1) { CaptureKeyBind(false); Sleep(30); continue; }
        if (cap == 2) { CapturePadBind();      Sleep(30); continue; }
        if (cap == 3) { CaptureKeyBind(true);  Sleep(30); continue; }

        // Recenter key. Not while the menu is open: the menu has its own button, and the letter
        // keys are menu navigation there.
        if (g_recenterVk && !OLMenu_Visible() && Bare((int)g_recenterVk, prec)) {
            Log("[OLVR][RECENTER] %s pressed", OLProxy_KeyName((int)g_recenterVk));
            OLXR_Recenter();
        }
        // Controller: two presses of the bound button inside the double-press window.
        if (g_recenterPad) {
            const bool padDown = (PadButtons() & (WORD)g_recenterPad) != 0;
            const bool padTap = padDown && !ppad;
            ppad = padDown;
            if (padTap) {
                const DWORD now = GetTickCount();
                if (lastPadTapMs && (now - lastPadTapMs) <= kPadDoubleMs) {
                    lastPadTapMs = 0;
                    Log("[OLVR][RECENTER] %s double press", OLProxy_PadName((int)g_recenterPad));
                    OLXR_Recenter();
                } else lastPadTapMs = now;
            }
        }

        if (g_menuVk && Bare((int)g_menuVk, pins)) OLMenu_Toggle();
        // [TOUCHMENU] Left grip + Menu opens and closes this menu from the VR controllers - the
        // keyboard is out of reach in a headset, and on a standalone headset there is none.
        {
            OLXrPad tp;
            const bool menuDown = OLXR_MotionControlsReady() && OLXR_GetPad(&tp) &&
                                  (tp.buttons & OLXR_BTN_MENU) != 0;
            const bool chord = menuDown && tp.gripL > 0.6f;
            if (chord && !g_menuChordLatch) {
                InterlockedExchange(&g_menuChordLatch, 1);
                OLMenu_Toggle();
                Log("[OLVR][TOUCHMENU] left grip + Menu -> mod menu %s", OLMenu_Visible() ? "open" : "closed");
            } else if (!menuDown && g_menuChordLatch) {
                InterlockedExchange(&g_menuChordLatch, 0);
            }
        }
        if (OLMenu_Visible()) {
            // Arrows are the primary control (the camera is frozen while this is open, so a
            // keyboard menu is fully usable). Also accept W/A/S/D + Enter, since a hand is
            // usually already there, and the gamepad d-pad maps to the arrows on most setups.
            if (Bare(VK_UP,    pup) || Bare('W', pw)) OLMenu_Nav(-1);
            if (Bare(VK_DOWN,  pdn) || Bare('S', ps)) OLMenu_Nav(+1);
            if (Bare(VK_LEFT,  plf) || Bare('A', pa)) OLMenu_Adjust(-1);
            if (Bare(VK_RIGHT, prt) || Bare('D', pd)) OLMenu_Adjust(+1);
            if (Bare(VK_RETURN, pen) || Bare(VK_SPACE, psp)) OLMenu_Adjust(+1);   // activate rows
        }
        // EVERY DEVELOPER HOTKEY IS GONE (2026-08-03, release). There used to be fourteen
        // CTRL+ALT chords here: stereo split on and off, the alternate-eye render mode, the
        // occlusion bypass, eye separation nudges, the eye-swap, the build-order flip, the
        // camera nudge, the layer-type flip, the UI mirror cycle and a fullscreen re-assert.
        // They were the tools that built this, and several of them visibly break the picture.
        // A player who finds one has no idea what it did and no way back, so they do not ship.
        // Everything a player actually needs is in the menu on INSERT, and the two recenter
        // bindings above. Anything left here must be something a stranger can press safely.

        // [PADSTUB] poll report every ~3 s, timed HERE rather than in the hook: a counter that
        // only advances when the hook runs cannot tell "the game stopped asking" from "the game
        // never asked", and that distinction is the entire experiment.
        if (++padTick >= 100) {
            padTick = 0;
            const long p0  = InterlockedExchange(&g_padPolls0, 0);
            const long pa  = InterlockedExchange(&g_padPollsAll, 0);
            const long fk  = InterlockedExchange(&g_padFaked, 0);
            if (p0 || pa || padReported) {
                padReported = true;
                Log("[OLVR][PADSTUB] slot0=%ld/3s allSlots=%ld/3s realPad=%s stub=%s faked=%ld/3s",
                    p0, pa, g_padRealOk ? "connected" : "absent",
                    g_padStub ? "on" : "off", fk);
            }
        }
        Sleep(30);
    }
}

// ---- [SETTINGS] tune in the headset, press one button, keep it -------------------------------
extern "C" float OLUE3_GetHalfEye();        extern "C" void OLUE3_SetHalfEye(float v);
extern "C" float OLUE3_GetWorldToMeters();  extern "C" void OLUE3_SetWorldToMeters(float v);
extern "C" int   OLUE3_GetHeadTrack();      extern "C" int  OLUE3_GetHeadPos();
extern "C" int   OLUE3_GetHeadRoll();       extern "C" float OLUE3_GetLeanGain();
extern "C" int   OLUE3_GetEyeSwap();        extern "C" void OLUE3_SetEyeSwap(int v);
extern "C" int   OLUE3_GetCineFov();
extern "C" int   OLUE3_GetNvLight();        extern "C" void OLUE3_SetNvLight(int v);
extern "C" float OLXR_GetConvergence();     extern "C" void OLXR_SetConvergence(float v);
extern "C" int   OLXR_GetStereoModel();     extern "C" void OLXR_SetStereoModel(int v);
extern "C" float OLXR_GetSmoothing();       extern "C" void OLXR_SetSmoothing(float v);
extern "C" float OLXR_GetGamma();           extern "C" void OLXR_SetGamma(float v);
extern "C" float OLXR_GetScreenBelow();     extern "C" void OLXR_SetScreenBelow(float v);
extern "C" int   OLXR_GetHandCam();         extern "C" float OLXR_GetHandCamStrength();
extern "C" void  OLUE3_GetArmFlags(int* handSync, int* leanFix, int* smoothDt);
extern "C" float OLUE3_GetWorldSize();   // (Beta 1.1) [WORLDSIZE]
extern "C" int   OLUE3_GetBodyTurn();    // (Beta 1.1) [BODYTURN]
extern "C" int OLUE3_GetLevelHorizon();
extern "C" int   OLUE3_GetLookUse();        extern "C" int OLUE3_GetWideFov();
extern "C" int   OLXR_GetUpscaler();        extern "C" float OLXR_GetSharpness();

// Everything worth tuning is tuned WEARING the headset, where writing a number down by hand is
// not an option. Save writes the live values to the same keys the loaders read, so save/restore
// is symmetric and the ini stays the single source of truth.
//
// UNITS WARNING (cost a near-miss 2026-08-03): Convergence here is THIS build's knob - slider
// -3..+3, one unit = 8% of the eye width, default 0.5. A later build used a different scale
// (-0.06..0.06) on the SAME key name, so an ini written by that build reads as ~zero
// convergence here and silently flattens the depth. The loader logs what it applied; if depth
// ever looks flat straight after a launch, read that line FIRST.
static bool IniPath(wchar_t* out, size_t n) {
    wchar_t exe[MAX_PATH] = { 0 };
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = 0;
    return swprintf_s(out, n, L"%soutlastvr.ini", exe) > 0;
}

static void WriteF(const wchar_t* sec, const wchar_t* key, float val, const wchar_t* ini) {
    wchar_t v[64]; swprintf_s(v, L"%.3f", val); WritePrivateProfileStringW(sec, key, v, ini);
}
static void WriteI(const wchar_t* sec, const wchar_t* key, long val, const wchar_t* ini) {
    wchar_t v[32]; swprintf_s(v, L"%ld", val); WritePrivateProfileStringW(sec, key, v, ini);
}

// The tunables no other loader already covers. Called after OLUE3_Init so its defaults cannot
// overwrite a saved value. A key that is absent leaves the built-in default alone.
static void LoadSavedSettings() {
    wchar_t ini[MAX_PATH]; if (!IniPath(ini, MAX_PATH)) return;
    wchar_t v[64];
    g_mouseUse = GetPrivateProfileIntW(L"Input", L"MouseUse", 1, ini) ? 1 : 0;
    Log("[OLVR][MOUSEUSE] %s (gameplay B=left mouse, X=crouch; menus/grip shortcuts unchanged)",
        g_mouseUse ? "ON" : "OFF: legacy layout");
    GetPrivateProfileStringW(L"Render", L"Convergence", L"", v, 64, ini);
    if (v[0]) OLXR_SetConvergence((float)_wtof(v));
    GetPrivateProfileStringW(L"Render", L"StereoModel", L"", v, 64, ini);
    if (v[0]) OLXR_SetStereoModel(_wtoi(v) ? 1 : 0);
    GetPrivateProfileStringW(L"Render", L"Smoothing", L"", v, 64, ini);
    if (v[0]) OLXR_SetSmoothing((float)_wtof(v));
    // Gamma is deliberately NOT read. It blacked out a headset, the control is gone from the
    // menu, and an ini key left over from an older build must not be able to bring it back.
    GetPrivateProfileStringW(L"Render", L"CinemaScreenBelowDeg", L"", v, 64, ini);
    if (v[0]) OLXR_SetScreenBelow((float)_wtof(v));
    GetPrivateProfileStringW(L"Stage3", L"EyeSwap", L"", v, 64, ini);
    if (v[0]) OLUE3_SetEyeSwap(_wtoi(v) ? 1 : 0);
    GetPrivateProfileStringW(L"Stage3", L"NvLight", L"", v, 64, ini);
    if (v[0]) OLUE3_SetNvLight(_wtoi(v) ? 1 : 0);
    { LONG s = (LONG)GetPrivateProfileIntW(L"Render", L"HudScalePct",   g_hudScalePct, ini);
      LONG x = (LONG)GetPrivateProfileIntW(L"Render", L"HudOffsetXPct", g_hudOffXPct,  ini);
      LONG y = (LONG)GetPrivateProfileIntW(L"Render", L"HudOffsetYPct", g_hudOffYPct,  ini);
      OLProxy_SetHudFit((float)s * 0.01f, (float)x * 0.01f, (float)y * 0.01f); }
    { LONG k = (LONG)GetPrivateProfileIntW(L"Input", L"RecenterKey",    g_recenterVk,  ini);
      LONG p = (LONG)GetPrivateProfileIntW(L"Input", L"RecenterPad",    g_recenterPad, ini);
      LONG m = (LONG)GetPrivateProfileIntW(L"Input", L"MenuKey",        g_menuVk,      ini);
      if (k >= 0 && k <= 0xFE) InterlockedExchange(&g_recenterVk, k);
      if (p >= 0 && p <= 0xFFFF) InterlockedExchange(&g_recenterPad, p);
      // A zero here would leave no way to open the menu at all, so it falls back rather than
      // being obeyed. This is the one binding that cannot be repaired from inside the game.
      if (m > 0 && m <= 0xFE) InterlockedExchange(&g_menuVk, m);
      else InterlockedExchange(&g_menuVk, VK_INSERT); }

    // FIRST RUN. Nobody reads a readme, so the first time this mod is ever launched the settings
    // menu opens itself once. That is the only way a player finds out it exists, what it can do,
    // and which key brings it back. The flag is written immediately, not on exit, so a crash or
    // an alt-F4 during that first session still counts as shown.
    g_firstRunPending = GetPrivateProfileIntW(L"Menu", L"Introduced", 0, ini) ? 0 : 1;
    if (g_firstRunPending) {
        WriteI(L"Menu", L"Introduced", 1, ini);
        Log("[OLVR][MENU] first run on this install: the settings menu will open once by itself");
    }
    Log("[OLVR][SETTINGS] loaded: IPD half=%.2fuu conv=%.2f (this build: -3..3, 8%%/unit) "
        "smooth=%.2f gamma=%.2f hud=%ld%%(%ld%%,%ld%%)",
        OLUE3_GetHalfEye(), OLXR_GetConvergence(), OLXR_GetSmoothing(), OLXR_GetGamma(),
        g_hudScalePct, g_hudOffXPct, g_hudOffYPct);
}

// [PADSTUB] Install the counter/stub hook. Hooks the EXPORT by ordinal, not an IAT entry by
// name: OLGame.exe imports XINPUT1_3.dll ordinals 2 and 3 only, with no name imports at all
// (dumpbin, 2026-08-17), so a name-based IAT patch - the usual approach, and what CP2077's
// InstallXInputHook does - would find nothing to patch here.
// The mod's own pad readers (menu navigation, the recenter double-tap) load xinput1_4 first, a
// different module, so they are not affected by this and keep seeing the raw device.
extern "C" void OLProxy_InstallPadProbe() {
    wchar_t ini[MAX_PATH];
    if (IniPath(ini, MAX_PATH))
        g_padStub = GetPrivateProfileIntW(L"Stage3", L"PadStub", 0, ini) ? 1 : 0;
    HMODULE m = GetModuleHandleW(L"xinput1_3.dll");
    if (!m) {
        Log("[OLVR][PADSTUB] xinput1_3.dll is not loaded in this process - the game has not "
            "touched XInput at all");
        return;
    }
    void* fn = (void*)GetProcAddress(m, MAKEINTRESOURCEA(2));   // ordinal 2 = XInputGetState
    if (!fn) { Log("[OLVR][PADSTUB] xinput1_3 ordinal 2 not found"); return; }
    MH_STATUS ms = MH_Initialize();   // another site may already have done this
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) {
        Log("[OLVR][PADSTUB] MH_Initialize failed (%d)", (int)ms); return;
    }
    if (MH_CreateHook(fn, (void*)&Hook_XiGetState, (void**)&o_XiGetState) == MH_OK &&
        MH_EnableHook(fn) == MH_OK) {
        Log("[OLVR][PADSTUB] watching the game's XInput reads (stub %s). This build answers "
            "for an EMPTY slot 0 only; a real controller always wins.",
            g_padStub ? "ON - empty slot 0 will walk forward 1.5s / back 1.5s" : "off, counting only");
    } else {
        Log("[OLVR][PADSTUB] hook failed");
    }
    void* setFn = (void*)GetProcAddress(m, MAKEINTRESOURCEA(3));   // ordinal 3 = XInputSetState
    if (setFn && MH_CreateHook(setFn, (void*)&Hook_XiSetState, (void**)&o_XiSetState) == MH_OK &&
        MH_EnableHook(setFn) == MH_OK)
        Log("[OLVR][HAPTICS] watching the game's rumble (XInputSetState) - forwarded to the VR controllers");
    else
        Log("[OLVR][HAPTICS] XInputSetState hook unavailable - controllers will not vibrate");
}

extern "C" int  OLUE3_GetFilmGrain();   // (Test47) [GRAIN]
extern "C" int OLProxy_SaveSettings() {
    wchar_t ini[MAX_PATH]; if (!IniPath(ini, MAX_PATH)) return 0;
    WriteF(L"Stage3", L"HalfEyeUU",     OLUE3_GetHalfEye(),       ini);
    WriteF(L"Render", L"WorldSize",     OLUE3_GetWorldSize(),     ini);   // (Beta 1.1) [WORLDSIZE]
    WriteF(L"Stage3", L"WorldToMeters", OLUE3_GetWorldToMeters(), ini);
    WriteI(L"Stage3", L"HeadTracking",  OLUE3_GetHeadTrack(),     ini);
    WriteI(L"Stage3", L"HeadPosition",  OLUE3_GetHeadPos(),       ini);
    WriteF(L"Stage3", L"LeanGain",      OLUE3_GetLeanGain(),      ini);
    WriteI(L"Stage3", L"HeadRoll",      OLUE3_GetHeadRoll(),      ini);
    WriteI(L"Stage3", L"EyeSwap",       OLUE3_GetEyeSwap(),       ini);
    WriteI(L"Stage3", L"CineFov",       OLUE3_GetCineFov(),       ini);
    WriteI(L"Stage3", L"NvLight",       OLUE3_GetNvLight(),       ini);
    WriteF(L"Render", L"Convergence",   OLXR_GetConvergence(),    ini);
    WriteI(L"Render", L"StereoModel",   OLXR_GetStereoModel(),    ini);
    WriteF(L"Render", L"Smoothing",     OLXR_GetSmoothing(),      ini);
    WriteF(L"Render", L"CinemaScreenBelowDeg", OLXR_GetScreenBelow(), ini);
    WriteI(L"Render", L"UiMirror",      g_uiMirror,               ini);
    WriteI(L"Render", L"HudScalePct",   g_hudScalePct,            ini);
    WriteI(L"Render", L"HudOffsetXPct", g_hudOffXPct,             ini);
    WriteI(L"Render", L"HudOffsetYPct", g_hudOffYPct,             ini);
    WriteI(L"Input",  L"MouseUse", OLProxy_MouseUseEnabled(), ini);
    WriteI(L"Input",  L"HandCamcorder", OLXR_GetHandCam(),        ini);
    WriteF(L"Input",  L"HandCamcorderStrength", OLXR_GetHandCamStrength(), ini);
    WriteI(L"Input",  L"RecenterKey",   g_recenterVk,             ini);
    WriteI(L"Input",  L"RecenterPad",   g_recenterPad,            ini);
    WriteI(L"Input",  L"MenuKey",       g_menuVk,                 ini);
    WriteI(L"Input",  L"Haptics",       OLXR_GetHaptics(),        ini);
    WriteF(L"Input",  L"HapticStrength", OLXR_GetHapticStrength(), ini);
    { int hs = 0, lf = 0, sd = 0; OLUE3_GetArmFlags(&hs, &lf, &sd);
      WriteI(L"Stage3", L"ArmHandSync", hs, ini);
      WriteI(L"Stage3", L"ArmLeanFix",  lf, ini);
      WriteI(L"Stage3", L"ArmSmoothDt", sd, ini); }
    WritePrivateProfileStringW(L"Motion", L"BodyFollowsHead", OLUE3_GetBodyTurn() ? L"1" : L"0", ini);   // (Beta 1.1)
    WriteI(L"Comfort", L"LevelHorizon",  OLUE3_GetLevelHorizon(), ini);
    OLMotion_Save(ini);
    OLInteract_Save(ini);   // [INTERACT]
    WriteI(L"Input",  L"LookToUse",     OLUE3_GetLookUse(),       ini);
    WriteI(L"Render", L"WideFov",       OLUE3_GetWideFov(),       ini);
    WriteI(L"Render", L"FilmGrain",     OLUE3_GetFilmGrain(),     ini);   // (Test47) [GRAIN]
    WriteI(L"XR",     L"Upscaler",      OLXR_GetUpscaler(),       ini);
    WriteF(L"XR",     L"Sharpness",     OLXR_GetSharpness(),      ini);
    WriteF(L"Render", L"RenderScale",   OLUE3_GetRenderScale(),   ini);
    Log("[OLVR][SETTINGS] SAVED as defaults: depth=%.2f focus=%.2f steadying=%.2f "
        "hud=%ld%%(%ld%%,%ld%%)", OLUE3_GetHalfEye(), OLXR_GetConvergence(), OLXR_GetSmoothing(),
        g_hudScalePct, g_hudOffXPct, g_hudOffYPct);
    return 1;
}

// The stereo work happens in the GAME's renderer; the headset is just a consumer of it. When
// something is wrong in the rendered frame there is no reason to wear a headset to look at it,
// so XR can be switched off entirely and the whole thing debugged on the monitor.
static void ReadXrConfig() {
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = 0;
    wchar_t ini[MAX_PATH]; swprintf_s(ini, L"%soutlastvr.ini", exe);
    wchar_t val[32] = { 0 };
    GetPrivateProfileStringW(L"XR", L"Enabled", L"1", val, 32, ini);
    g_xrAllowed = (_wcsicmp(val, L"0") == 0 || _wcsicmp(val, L"false") == 0) ? 0 : 1;
    Log("[OLVR] XR %s (outlastvr.ini [XR] Enabled)", g_xrAllowed ? "ENABLED" : "DISABLED - flat, no headset needed");
    // [PACING] In VR the game must present on the HEADSET's clock (xrWaitFrame), not the
    // monitor's. With the game's own vsync live there are TWO serial blocking waits on two
    // drifting clocks, the submit slides past the compositor's deadline on the beat frequency,
    // and SteamVR logs it as endless frame timeouts (510 in a 16s session, measured 2026-08-04)
    // while cross-fading the game semi-transparent into its backdrop - it never leaves its
    // startup fade. So when XR is on, the game's vsync is forced OFF at CreateDevice/Reset and
    // xrWaitFrame becomes the only pacer. [XR] KeepGameVsync=1 restores the old behaviour.
    g_keepVsync = GetPrivateProfileIntW(L"XR", L"KeepGameVsync", 0, ini) ? 1 : 0;
    if (g_xrAllowed)
        Log("[OLVR][PACING] game vsync %s (headset clock %s the pacer)",
            g_keepVsync ? "KEPT by ini - two clocks will fight" : "forced OFF",
            g_keepVsync ? "is NOT" : "is");
    { LONG m = (LONG)GetPrivateProfileIntW(L"Render", L"UiMirror", UIMIRROR_ON, ini);
      g_uiMirror = (m >= 0 && m <= 2) ? m : UIMIRROR_ON; }
    Log("[OLVR][UIMIRROR] mode=%ld (0=off 1=on 2=menus-only), from the ini", g_uiMirror);
    g_desktopMirrorOneEye = GetPrivateProfileIntW(L"Render", L"DesktopMirrorOneEye", 1, ini) ? 1 : 0;
    OLProxy_SetOcclSkip(GetPrivateProfileIntW(L"Render", L"OcclusionSkip", 1, ini) ? 1 : 0);
    { const int mf = (int)GetPrivateProfileIntW(L"Render", L"DesktopMirrorFps", 30, ini); g_mirrorFps = mf < 0 ? 0 : (mf > 0 && mf < 10 ? 10 : mf); }
    Log("[OLVR][MIRRORFPS] the monitor shows %ld frames a second during play (0 = every frame)", (long)g_mirrorFps);
    Log("[OLVR][MIRROR] desktop preview=%s", g_desktopMirrorOneEye ? "single left eye" : "raw side-by-side");
}

// Must run BEFORE the game creates its device, so it is called from the attach-time worker.
static void InstallDisplayHooks() {
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = 0;
    wchar_t ini[MAX_PATH]; swprintf_s(ini, L"%soutlastvr.ini", exe);
    g_reqW = (LONG)GetPrivateProfileIntW(L"Render", L"ResX", 0, ini);
    g_reqH = (LONG)GetPrivateProfileIntW(L"Render", L"ResY", 0, ini);
    g_autoFs = GetPrivateProfileIntW(L"Render", L"AutoFullscreen", 1, ini) ? 1 : 0;   // [FSFIX]
    g_highResWindow = GetPrivateProfileIntW(L"Render", L"HighResWindow", 1, ini) ? 1 : 0;
    Log("[OLVR][FSFIX] AutoFullscreen %s (re-assert exclusive-fullscreen after the compositor "
        "drops it)", g_autoFs ? "ON" : "off");
    Log("[OLVR][SCREQ] HighResWindow %s (oversized borderless rendering for headset sharpness)",
        g_highResWindow ? "ON" : "off");

    const LONG realW = GetSystemMetrics(SM_CXSCREEN), realH = GetSystemMetrics(SM_CYSCREEN);
    // RESOLUTION IS AUTOMATIC, and there is genuinely no choice to offer. The desktop size is a
    // hard ceiling here: this engine sizes its backbuffer from its own window client rect and
    // ignores every display query, and forcing a buffer above the desktop was measured to come
    // back at a broken aspect ratio, stretched and worse than 4K in every way. Below the desktop
    // is simply a worse picture. So the right number is always "the desktop", and asking a player
    // to pick it would be a menu of one good answer and a list of downgrades.
    // ResX/ResY remain readable for the one real case, a machine that cannot hold framerate at
    // full size, and for testing. Absent or zero means auto.
    if (g_reqW <= 0 || g_reqH <= 0) {
        g_reqW = realW; g_reqH = realH;
        Log("[OLVR][SCREQ] resolution AUTO -> matching the desktop, %ldx%ld", g_reqW, g_reqH);
    } else {
        Log("[OLVR][SCREQ] resolution from the ini: %ldx%ld (desktop is %ldx%ld)", g_reqW, g_reqH, realW, realH);
    }
    if (g_reqW <= 0 || g_reqH <= 0) { Log("[OLVR][SCREQ] no usable desktop size -> resolution untouched"); return; }
    // Arm whenever a size is asked for. Do NOT compare against the primary here: the game may be
    // windowed on a SMALLER second monitor (it was), so a primary-sized request still matters.
    // The per-monitor check inside the hook keeps big-enough displays untouched.
    if (MH_Initialize() == MH_ERROR_MEMORY_ALLOC) { Log("[OLVR][SCREQ] MH_Initialize failed"); return; }
    bool ok = MH_CreateHookApi(L"user32", "GetMonitorInfoW", (void*)&Hook_GetMonitorInfoW, (void**)&o_GetMonitorInfoW) == MH_OK
           && MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    if (ok && MH_CreateHookApi(L"user32", "GetMonitorInfoA", (void*)&Hook_GetMonitorInfoA, (void**)&o_GetMonitorInfoA) == MH_OK)
        MH_EnableHook(MH_ALL_HOOKS);
    g_spoofArmed = ok ? 1 : 0;
    Log("[OLVR][SCREQ] primary-monitor spoof %s -> the game should size its windowed backbuffer %ldx%ld (%ldx%ld per eye)",
        ok ? "ARMED" : "FAILED", g_reqW, g_reqH, g_reqW / 2, g_reqH);
}

static DWORD WINAPI Worker(LPVOID) {
    OpenLog();
    Log("[OLVR] ==== Outlast FlatXR probe (Stage 2: flat panel in headset) ====");
    Log("[OLVR] built=" __DATE__ " " __TIME__ " bitness=x64");
    wchar_t exe[MAX_PATH] = { 0 }; GetModuleFileNameW(NULL, exe, MAX_PATH);
    Log("[OLVR] exe=%ls pid=%lu", exe, GetCurrentProcessId());
    Log("[OLVR] keys: INSERT opens the settings menu, G recenters; VR controllers: click both thumbsticks to recenter; gamepad: double tap the recenter button (L1 unless changed)");
    ReadXrConfig();
    InstallDisplayHooks();   // BEFORE the game's CreateDevice, or the clamp has already happened
    CreateThread(nullptr, 0, KeyThread, nullptr, 0, nullptr);
    OLUE3_Init();   // Stage 3 engine hook; no-ops unless outlastvr.ini arms it
    LoadSavedSettings();   // AFTER OLUE3_Init, so saved values are not overwritten by its defaults
    OLMotion_Init();       // [PHASE1] turning, walking and physical crouch settings
    OLInteract_Init();     // [INTERACT] doors, raise-to-eye, battery gesture, viewfinder, markers
    { wchar_t ini[MAX_PATH]; if (IniPath(ini, MAX_PATH)) OLDev_Init(ini); }   // [DEVBRIDGE]
    OLProxy_InstallPadProbe();   // [PADSTUB] motion controls step 1: does the game read a fabricated pad
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        InitializeCriticalSection(&g_cs);
        OLMenu_StartInput(hInst);   // low-level mouse hook needs the module's HINSTANCE
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        OLProxy_MouseUseShutdown();
        OLXR_Shutdown();
        if (g_log) {
            Log("[OLVR] ==== detach: presents=%ld ====", g_presentCount);
            // [ASYNCLOG] the writer thread is gone by now: the rest goes out here (no CRT lock - it may have
            // died holding it)
            if (g_logLen && g_logBuf) { _fwrite_nolock(g_logBuf, 1, g_logLen, g_log); g_logLen = 0; }
            _fflush_nolock(g_log); g_log = nullptr;
        }
    }
    return TRUE;
}
