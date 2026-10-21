// ol_devbridge.cpp - [DEVBRIDGE] a test harness for the developer, OFF unless [Debug] DevBridge=1.
// =============================================================================
// The mod is built and tested on a machine where the developer cannot wear the headset, so a
// change has to be confirmed some other way. This lets a script outside the game drive it and
// look at it:
//   %LOCALAPPDATA%\OutlastVR\dev\cmd.txt   - commands, one per line; consumed when read
//   %LOCALAPPDATA%\OutlastVR\dev\*.png     - snapshots of the frame exactly as the headset gets it
// Commands:
//   snap <name>             save the backbuffer (after the mod menu is drawn, before the XR
//                           capture) as dev\<name>.png
//   xrsnap <name>           save the headset bridge's copy of the frame as dev\<name>.png
//   menu                    toggle the mod settings menu
//   pad <BTN[+BTN]> <ms>    hold virtual-pad buttons: A B X Y START BACK UP DOWN LEFT RIGHT LB RB
//                           LS RS (L3/R3)
//   stick <lx> <ly> <rx> <ry> <ms>   hold the virtual sticks (-1..1)
//   key <vk> <ms>           press a keyboard key (decimal virtual-key code) in the game window
//   mark <text>             write a marker into OutlastVR_Log.txt
//   objs <text>             log live objects whose name contains the text
//   cls <Class>             log a class's super chain and its own fields with offsets
//   get <Class> <field>     log that field in every live instance of the class
//   scale <f>               set the render scale live (0.4 - 1.5)
//   xrpub <0|1>             pause/resume publishing frames to the headset (measures their cost)
//   xrup <0|1>              skip/resume the bridge's upload of published frames (same, other half)
//   hands <0|1>             fake both controllers in front of the body (tests the arm solver on a
//                           headset without controllers)
//   lensfit <0|1>           each eye rendered with its own lens frustum on/off (A/B its cost)
//   latch <0|1>             [FRAMETAG] per-frame pose latch on/off (A/B the old Present-time rule)
//   headwave <0|1>          a steady synthetic head turn (makes pose pairing errors measurable)
//   look <yaw> <pitch>      hold the head turned/tilted (degrees, + = left / up)
//   occl <0|1>              force every occlusion query to 'visible' (the flicker fix) on/off
//   hand <L|R> x y z [ms]   glide a fake controller to (x,y,z) m from the head over ms (needs hands 1)
//                           +x right, +y up, -z forward
//   grip <L|R> <0..1>       close/open a fake controller's grip
//   setf <hexaddr> <f>      write a float into game memory (SEH-guarded; research only)
//   tp <hexactor> [dz]      move the player's pawn to that actor's Location (+dz uu up)
//   oget <hexaddr> <field>  log one field of one object (found through its class chain)
//   raw <hexaddr> [bytes]   log memory as qwords, naming any that point at objects
//   hide <hexaddr> <0|1>    SetHiddenGame on a primitive component (runs on the game thread)
//   camtune <0|1|2> a b c   [HOLSTER] handycam: 0 held rot (deg), 1 held offset (uu), 2 chest rot, 3 chest offset (up fwd right uu)
//   vrbtn <A+B+X+Y+MENU+LS+RS> <ms>   scripted Quest controller buttons (through the mod's mapping)
//   trig <L|R> <0..1>       scripted trigger
//   bones                   Miles's skeleton: index, name, parent
//   gripdump                the captured native camcorder grip (finger locals)
//   dgrip <L|R> <w>         force a hand's door grip (0..1; -1 = back to normal)
//   headoff x y z           add a fixed offset (m, recenter frame) to the head - "the player moved"
// Polled from Present every 15 frames; nothing here runs at all when the ini switch is off.
// =============================================================================
#include <Windows.h>
#include <d3d9.h>
#include <wincodec.h>
#include <ShlObj.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "ol_menu.h"

extern "C" void OLProxyLog(const char* line);
extern "C" void OLUE3_DevDumpObjects(const char* sub);
extern "C" void OLUE3_DevClass(const char* name);
extern "C" void OLUE3_DevGet(const char* args);
extern "C" void OLUE3_DevEnum(const char* args);
extern "C" int  OLUE3_SetRenderScale(float s);
extern "C" void OLXR_DevSetPublish(int on);
extern "C" void OLXR_DevSetUpload(int on);
extern "C" void OLXR_DevSnap(const wchar_t* path);
extern "C" IDirect3DTexture9* OLProxy_LensTex();                                 // (Test46) [LENSCAM]
extern "C" void OLUE3_DevConsole(const char* c);                                    // (Test46) [DEVBRIDGE] con
extern "C" void OLProxy_SetMirrorFps(int f);                                        // (Test46) [MIRRORFPS]
extern "C" void OLProxy_SetOcclSkip(int on);                                        // (Test46) [OCCLSKIP]
extern "C" void OLUE3_SetFilmGrain(int on);                                         // (Test47) [GRAIN]
extern "C" void OLXR_DevSetFakeHands(int on);
extern "C" void OLXR_SetLensFit(int on);
extern "C" void OLXR_DevSetLatch(int on);
extern "C" void OLXR_DevSetHeadWave(int on);
extern "C" void OLXR_DevLook(float yawDeg, float pitchDeg);
extern "C" void OLUE3_SetOcclForce(int on);
extern "C" void OLXR_DevHandTo(int hand, float x, float y, float z, int ms);
extern "C" void OLXR_DevGrip(int hand, float v);
extern "C" void OLXR_DevHandRot(int hand, float pitch, float yaw, float roll);   // (Test40)
extern "C" void OLUE3_DevCamHold(int mode);                                        // (Test40)
extern "C" void OLUE3_DevLiveScreen(int what);                                     // (Test46) [LIVESCREEN]
extern "C" void OLXR_DevStick(float lx, float ly, float rx, float ry, int ms);      // (Test46)
extern "C" void OLUE3_DevLensOff(float x, float y, float z);
extern "C" void OLUE3_DevLiveScreenTex(const char* name);
extern "C" void OLUE3_DevLiveScreenTarget(const char* name);
extern "C" void OLUE3_DevLiveScreenOp(int op, const char* a, const char* b);
extern "C" void OLInteract_DevLedges();                                            // (Test40)
extern "C" void OLInteract_DevDoors();                                             // (Test40)
extern "C" void OLInteract_DevFakeLedge(float upCm);                               // (Test40)
extern "C" void OLInteract_DevLadderBlock(float uu);                               // (Test41)
extern "C" void OLUE3_DevArmFix(int mask);                                         // (Test42)
extern "C" void OLInteract_SetClimbMode(int m);                                    // (Test43)
extern "C" void OLInteract_DevDoorEvent(int type);
extern "C" void OLUE3_DevFlipScreen(const float v[6]);
extern "C" void OLUE3_DevScissorFix(int m);
extern "C" int  OLUE3_HeroSetPhysics(int phys);
extern "C" int  OLUE3_HeroMove(const float d[3], float out[3]);
extern "C" void OLUE3_DevTeleport(void* actor, float dz);
extern "C" void OLUE3_DevTeleportTo(float x, float y, float z);
extern "C" void OLUE3_DevObjGet(const char* args);
extern "C" void OLUE3_DevRaw(const char* args);
extern "C" void OLUE3_DevQueueHide(void* comp, int hidden);
extern "C" void OLUE3_DevCamTune(int which, float a, float b, float c);
extern "C" void OLXR_DevButtons(unsigned mask, int ms);
extern "C" void OLXR_DevTrigger(int hand, float v);
extern "C" void OLUE3_DevBones();
extern "C" void OLUE3_DevGripDump();
extern "C" void OLInteract_DevGripOverride(int h, float w);
extern "C" void OLXR_DevHeadOffset(float x, float y, float z);
extern "C" void OLUE3_DevCurl(float t0, float t1, float t2, float f0, float f1, float f2);

static long g_devOn = 0;
static wchar_t g_devDir[MAX_PATH] = { 0 };
static volatile ULONGLONG g_padUntil = 0, g_stickUntil = 0;
static volatile WORD  g_padButtons = 0;
static volatile float g_stick[4] = { 0, 0, 0, 0 };

extern "C" int OLDev_Enabled() { return g_devOn ? 1 : 0; }

extern "C" void OLDev_Init(const wchar_t* ini) {
    g_devOn = GetPrivateProfileIntW(L"Debug", L"DevBridge", 0, ini) ? 1 : 0;
    if (!g_devOn) return;
    PWSTR base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base) {
        swprintf_s(g_devDir, L"%s\\OutlastVR\\dev", base);
        CoTaskMemFree(base);
        CreateDirectoryW(g_devDir, nullptr);
    }
    OLProxyLog("[OLVR][DEVBRIDGE] ON - commands from %LOCALAPPDATA%\\OutlastVR\\dev\\cmd.txt");
}

// Virtual-pad override for the XInput hook. 0 = bridge off; 1 = on, nothing held (the hook still
// reports a connected, neutral pad: with no pad present UE3 only re-checks slot 0 every second or
// two, which a short scripted press would fall between); 2 = a pad/stick command is being held.
extern "C" int OLDev_Pad(WORD* buttons, float* lx, float* ly, float* rx, float* ry) {
    if (!g_devOn) return 0;
    const ULONGLONG now = GetTickCount64();
    int active = 1;
    if (g_padUntil && now < g_padUntil) { *buttons |= g_padButtons; active = 2; }
    if (g_stickUntil && now < g_stickUntil) {
        *lx = g_stick[0]; *ly = g_stick[1]; *rx = g_stick[2]; *ry = g_stick[3]; active = 2;
    }
    return active;
}

static WORD ParseButtons(const char* s) {
    static const struct { const char* n; WORD m; } k[] = {
        { "UP", 0x0001 }, { "DOWN", 0x0002 }, { "LEFT", 0x0004 }, { "RIGHT", 0x0008 },
        { "START", 0x0010 }, { "BACK", 0x0020 }, { "LS", 0x0040 }, { "RS", 0x0080 },
        { "LB", 0x0100 }, { "RB", 0x0200 }, { "A", 0x1000 }, { "B", 0x2000 }, { "X", 0x4000 }, { "Y", 0x8000 } };
    WORD m = 0;
    char buf[128]; strncpy_s(buf, s, _TRUNCATE);
    char* ctx = nullptr;
    for (char* t = strtok_s(buf, "+", &ctx); t; t = strtok_s(nullptr, "+", &ctx))
        for (auto& e : k) if (!_stricmp(t, e.n)) m |= e.m;
    return m;
}

static bool SaveSurfacePng(IDirect3DDevice9* dev, IDirect3DSurface9* bb, const wchar_t* path);
static bool SavePng(IDirect3DDevice9* dev, const wchar_t* path) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    return SaveSurfacePng(dev, bb, path);   // releases bb
}
// (Test46) any render-target surface (the backbuffer, the camcorder lens); takes over the reference
static bool SaveSurfacePng(IDirect3DDevice9* dev, IDirect3DSurface9* bb, const wchar_t* path) {
    D3DSURFACE_DESC sd; bb->GetDesc(&sd);
    IDirect3DSurface9* sys = nullptr;
    bool ok = false;
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(bb, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            IWICImagingFactory* f = nullptr;
            const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&f))) && f) {
                IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
                if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) &&
                    SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                    SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
                    SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
                    SUCCEEDED(fr->SetSize(sd.Width, sd.Height))) {
                    // The PNG encoder may answer SetPixelFormat with a different format (24bpp BGR
                    // here) - writing 32-bit rows into that skewed every row by 4/3 and rotated
                    // the channels. Always hand it tightly packed 24-bit BGR.
                    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
                    fr->SetPixelFormat(&pf);
                    if (IsEqualGUID(pf, GUID_WICPixelFormat24bppBGR)) {
                        const UINT rowOut = sd.Width * 3;
                        BYTE* px = (BYTE*)malloc((size_t)rowOut * sd.Height);
                        if (px) {
                            for (UINT y = 0; y < sd.Height; ++y) {
                                const BYTE* s = (const BYTE*)lr.pBits + (size_t)y * lr.Pitch;
                                BYTE* d = px + (size_t)y * rowOut;
                                for (UINT x = 0; x < sd.Width; ++x) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d += 3; s += 4; }
                            }
                            ok = SUCCEEDED(fr->WritePixels(sd.Height, rowOut, rowOut * sd.Height, px)) &&
                                 SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
                            free(px);
                        }
                    }
                }
                if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release();
                f->Release();
            }
            if (SUCCEEDED(ci)) CoUninitialize();
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    bb->Release();
    char l[400];
    sprintf_s(l, "[OLVR][DEVBRIDGE] snap %ls %ux%u -> %s", path, sd.Width, sd.Height, ok ? "saved" : "FAILED");
    OLProxyLog(l);
    return ok;
}

static void KeyPress(int vk, int ms) {
    HWND w = GetForegroundWindow();
    (void)w;
    INPUT in[1] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = (WORD)vk; in[0].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    SendInput(1, in, sizeof(INPUT));
    Sleep(ms > 0 && ms < 2000 ? ms : 80);
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, in, sizeof(INPUT));
}

struct KeyJob { int vk, ms; };
static DWORD WINAPI KeyThreadProc(LPVOID p) {
    KeyJob* j = (KeyJob*)p; KeyPress(j->vk, j->ms); delete j; return 0;
}

// Called from Present with the live device (after the mod menu is drawn).
extern "C" void OLDev_OnPresent(IDirect3DDevice9* dev, long presentCount) {
    if (!g_devOn || !g_devDir[0] || (presentCount % 15) != 0) return;
    wchar_t cmdPath[MAX_PATH]; swprintf_s(cmdPath, L"%s\\cmd.txt", g_devDir);
    if (GetFileAttributesW(cmdPath) == INVALID_FILE_ATTRIBUTES) return;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, cmdPath, L"rb") || !fp) return;
    char text[4096] = { 0 };
    const size_t n = fread(text, 1, sizeof(text) - 1, fp);
    fclose(fp);
    DeleteFileW(cmdPath);
    text[n] = 0;
    char* ctx = nullptr;
    for (char* line = strtok_s(text, "\r\n", &ctx); line; line = strtok_s(nullptr, "\r\n", &ctx)) {
        char cmd[32] = { 0 }, a[260] = { 0 };
        if (sscanf_s(line, "%31s %259[^\n]", cmd, (unsigned)sizeof(cmd), a, (unsigned)sizeof(a)) < 1) continue;
        char l[400]; sprintf_s(l, "[OLVR][DEVBRIDGE] cmd: %s", line); OLProxyLog(l);
        if (!_stricmp(cmd, "snap")) {
            wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\%hs.png", g_devDir, a[0] ? a : "snap");
            SavePng(dev, path);
        } else if (!_stricmp(cmd, "fingerless")) {           // (Test47) Miles's post-Trager body
            OLUE3_DevLiveScreenOp(19, "", "");
        } else if (!_stricmp(cmd, "grain")) {                // (Test47) [GRAIN] 0 | 1
            OLUE3_SetFilmGrain(atoi(a));
        } else if (!_stricmp(cmd, "occlskip")) {             // (Test46) [OCCLSKIP] 0 | 1
            OLProxy_SetOcclSkip(atoi(a));
        } else if (!_stricmp(cmd, "mirrorfps")) {            // (Test46) [MIRRORFPS] monitor presents a second, 0 = all
            OLProxy_SetMirrorFps(atoi(a));
        } else if (!_stricmp(cmd, "con")) {                  // (Test46) con <console command>
            OLUE3_DevConsole(a);
        } else if (!_stricmp(cmd, "lenssnap")) {             // (Test46) [LENSCAM] the camcorder lens's picture
            IDirect3DTexture9* lt = OLProxy_LensTex(); IDirect3DSurface9* ls = nullptr;
            wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\%hs.png", g_devDir, a[0] ? a : "lens");
            if (lt && SUCCEEDED(lt->GetSurfaceLevel(0, &ls)) && ls) SaveSurfacePng(dev, ls, path);
            else OLProxyLog("[OLVR][DEVBRIDGE] lenssnap: no lens render target yet");
        } else if (!_stricmp(cmd, "xrsnap")) {
            wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\%hs.png", g_devDir, a[0] ? a : "xrsnap");
            OLXR_DevSnap(path);
        } else if (!_stricmp(cmd, "menu")) {
            OLMenu_Toggle();
        } else if (!_stricmp(cmd, "pad")) {
            char b[128] = { 0 }; int ms = 150;
            sscanf_s(a, "%127s %d", b, (unsigned)sizeof(b), &ms);
            g_padButtons = ParseButtons(b);
            g_padUntil = GetTickCount64() + (ULONGLONG)(ms > 0 ? ms : 150);
        } else if (!_stricmp(cmd, "stick")) {
            float v[4] = { 0, 0, 0, 0 }; int ms = 300;
            sscanf_s(a, "%f %f %f %f %d", &v[0], &v[1], &v[2], &v[3], &ms);
            for (int i = 0; i < 4; ++i) g_stick[i] = v[i];
            g_stickUntil = GetTickCount64() + (ULONGLONG)(ms > 0 ? ms : 300);
        } else if (!_stricmp(cmd, "key")) {
            int vk = 0, ms = 80;
            sscanf_s(a, "%d %d", &vk, &ms);
            if (vk > 0 && vk < 256) CreateThread(nullptr, 0, KeyThreadProc, new KeyJob{ vk, ms }, 0, nullptr);
        } else if (!_stricmp(cmd, "objs")) {
            OLUE3_DevDumpObjects(a);
        } else if (!_stricmp(cmd, "cls")) {
            OLUE3_DevClass(a);
        } else if (!_stricmp(cmd, "get")) {
            OLUE3_DevGet(a);
        } else if (!_stricmp(cmd, "enum")) {
            OLUE3_DevEnum(a);
        } else if (!_stricmp(cmd, "scale")) {
            const float s = (float)atof(a);
            const int changed = OLUE3_SetRenderScale(s);
            sprintf_s(l, "[OLVR][DEVBRIDGE] render scale -> %.2f (%s)", s, changed ? "resizing" : "unchanged"); OLProxyLog(l);
        } else if (!_stricmp(cmd, "xrpub")) {
            OLXR_DevSetPublish(atoi(a));
            sprintf_s(l, "[OLVR][DEVBRIDGE] XR frame publishing %s", atoi(a) ? "ON" : "PAUSED"); OLProxyLog(l);
        } else if (!_stricmp(cmd, "xrup")) {
            OLXR_DevSetUpload(atoi(a));
            sprintf_s(l, "[OLVR][DEVBRIDGE] bridge upload %s", atoi(a) ? "ON" : "SKIPPED"); OLProxyLog(l);
        } else if (!_stricmp(cmd, "hands")) {
            OLXR_DevSetFakeHands(atoi(a));
            sprintf_s(l, "[OLVR][DEVBRIDGE] fake controllers %s", atoi(a) ? "ON" : "OFF"); OLProxyLog(l);
        } else if (!_stricmp(cmd, "lensfit")) {
            OLXR_SetLensFit(atoi(a));
            sprintf_s(l, "[OLVR][DEVBRIDGE] lens-shaped eyes %s", atoi(a) ? "ON" : "OFF"); OLProxyLog(l);
        } else if (!_stricmp(cmd, "look")) {
            float yw = 0, pt = 0; sscanf_s(a, "%f %f", &yw, &pt); OLXR_DevLook(yw, pt);
            sprintf_s(l, "[OLVR][DEVBRIDGE] head held at yaw %.0f pitch %.0f deg", yw, pt); OLProxyLog(l);
        } else if (!_stricmp(cmd, "headwave")) {
            OLXR_DevSetHeadWave(atoi(a));
            sprintf_s(l, "[OLVR][DEVBRIDGE] synthetic head turn %s", atoi(a) ? "ON" : "OFF"); OLProxyLog(l);
        } else if (!_stricmp(cmd, "occl")) {
            OLUE3_SetOcclForce(atoi(a));
            sprintf_s(l, "[OLVR][DEVBRIDGE] occlusion results forced visible: %s", atoi(a) ? "ON" : "OFF (game culls)"); OLProxyLog(l);
        } else if (!_stricmp(cmd, "hand")) {
            char side[8] = { 0 }; float x = 0, y = 0, z = 0; int ms = 300;
            if (sscanf_s(a, "%7s %f %f %f %d", side, (unsigned)sizeof(side), &x, &y, &z, &ms) >= 4) {
                const int h = (side[0] == 'R' || side[0] == 'r') ? 1 : 0;
                OLXR_DevHandTo(h, x, y, z, ms);
                sprintf_s(l, "[OLVR][DEVBRIDGE] %s hand -> (%.2f %.2f %.2f) m over %d ms", h ? "right" : "left", x, y, z, ms); OLProxyLog(l);
            }
        } else if (!_stricmp(cmd, "hrot")) {
            char side[8] = { 0 }; float pt = 0, yw = 0, rl = 0;
            if (sscanf_s(a, "%7s %f %f %f", side, (unsigned)sizeof(side), &pt, &yw, &rl) >= 2) {
                const int h = (side[0] == 'R' || side[0] == 'r') ? 1 : 0;
                OLXR_DevHandRot(h, pt, yw, rl);
                sprintf_s(l, "[OLVR][DEVBRIDGE] %s controller turned pitch %.0f yaw %.0f roll %.0f deg", h ? "right" : "left", pt, yw, rl); OLProxyLog(l);
            }
        } else if (!_stricmp(cmd, "camhold")) {
            OLUE3_DevCamHold(atoi(a));
        } else if (!_stricmp(cmd, "ledges")) {
            OLInteract_DevLedges();
        } else if (!_stricmp(cmd, "doors")) {
            OLInteract_DevDoors();
        } else if (!_stricmp(cmd, "fakeledge")) {             // [up cm above the eyes] (Test43; default 45 below)
            OLInteract_DevFakeLedge((float)atof(a));
        } else if (!_stricmp(cmd, "armfix")) {
            OLUE3_DevArmFix(atoi(a));
        } else if (!_stricmp(cmd, "climbmode")) {                  // (Test43) 0 A button, 1 guided, 2 physical
            OLInteract_SetClimbMode(atoi(a));
            sprintf_s(l, "[OLVR][DEVBRIDGE] climb mode %d", atoi(a)); OLProxyLog(l);
        } else if (!_stricmp(cmd, "doorev")) {                     // (Test43) fire one door event at the nearest door
            OLInteract_DevDoorEvent(atoi(a));
        } else if (!_stricmp(cmd, "flipscr")) {                    // (Test43) x y z (uu) turn (deg) w h (m)
            float v[6] = { 0 };
            if (sscanf_s(a, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) OLUE3_DevFlipScreen(v);
        } else if (!_stricmp(cmd, "scissorfix")) {                 // (Test43) 0 off, 1 moved, 2 the whole eye
            OLUE3_DevScissorFix(atoi(a));
        } else if (!_stricmp(cmd, "phys")) {                       // (Test43) set Miles's physics mode
            sprintf_s(l, "[OLVR][DEVBRIDGE] physics %d: %s", atoi(a), OLUE3_HeroSetPhysics(atoi(a)) ? "set" : "FAILED"); OLProxyLog(l);
        } else if (!_stricmp(cmd, "hmove")) {                      // (Test43) move Miles with collision
            float dv[3] = { 0 }, o[3] = { 0 };
            if (sscanf_s(a, "%f %f %f", &dv[0], &dv[1], &dv[2]) == 3) {
                const int ok = OLUE3_HeroMove(dv, o);
                sprintf_s(l, "[OLVR][DEVBRIDGE] move (%.0f %.0f %.0f): %s, now at (%.0f %.0f %.0f)", dv[0], dv[1], dv[2], ok ? "done" : "FAILED", o[0], o[1], o[2]); OLProxyLog(l);
            }
        } else if (!_stricmp(cmd, "ladderblock")) {
            OLInteract_DevLadderBlock((float)atof(a));
        } else if (!_stricmp(cmd, "grip")) {
            char side[8] = { 0 }; float v = 0;
            if (sscanf_s(a, "%7s %f", side, (unsigned)sizeof(side), &v) >= 2) {
                const int h = (side[0] == 'R' || side[0] == 'r') ? 1 : 0;
                OLXR_DevGrip(h, v);
                sprintf_s(l, "[OLVR][DEVBRIDGE] %s grip %.2f", h ? "right" : "left", v); OLProxyLog(l);
            }
        } else if (!_stricmp(cmd, "setf")) {
            unsigned long long addr = 0; float v = 0;
            if (sscanf_s(a, "%llx %f", &addr, &v) == 2 && addr > 0x10000) {
                bool ok = true;
                __try { *(volatile float*)(uintptr_t)addr = v; } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
                sprintf_s(l, "[OLVR][DEVBRIDGE] setf %llX = %.4f %s", addr, v, ok ? "written" : "FAULT"); OLProxyLog(l);
            }
        } else if (!_stricmp(cmd, "tp")) {
            unsigned long long addr = 0; float dz = 0, x = 0, y = 0, z = 0;
            if (!_strnicmp(a, "xyz", 3) && sscanf_s(a + 3, "%f %f %f", &x, &y, &z) == 3) OLUE3_DevTeleportTo(x, y, z);
            else if (sscanf_s(a, "%llx %f", &addr, &dz) >= 1 && addr > 0x10000) OLUE3_DevTeleport((void*)(uintptr_t)addr, dz);
        } else if (!_stricmp(cmd, "oget")) {
            OLUE3_DevObjGet(a);
        } else if (!_stricmp(cmd, "raw")) {
            OLUE3_DevRaw(a);
        } else if (!_stricmp(cmd, "vrbtn")) {
            char names[96] = { 0 }; int ms = 150; unsigned m = 0;
            sscanf_s(a, "%95s %d", names, (unsigned)sizeof(names), &ms);
            _strupr_s(names);
            if (strstr(names, "MENU")) m |= 1u << 6;
            if (strstr(names, "LS")) m |= 1u << 4;
            if (strstr(names, "RS")) m |= 1u << 5;
            for (char* q = names; *q; ++q) {
                const bool solo = (q == names || q[-1] == '+') && (q[1] == 0 || q[1] == '+');
                if (!solo) continue;
                if (*q == 'A') m |= 1u << 0; else if (*q == 'B') m |= 1u << 1; else if (*q == 'X') m |= 1u << 2; else if (*q == 'Y') m |= 1u << 3;
            }
            OLXR_DevButtons(m, ms);
            sprintf_s(l, "[OLVR][DEVBRIDGE] controller buttons 0x%X for %d ms", m, ms); OLProxyLog(l);
        } else if (!_stricmp(cmd, "trig")) {
            char side[8] = { 0 }; float v = 0;
            if (sscanf_s(a, "%7s %f", side, (unsigned)sizeof(side), &v) >= 2) {
                const int h = (side[0] == 'R' || side[0] == 'r') ? 1 : 0;
                OLXR_DevTrigger(h, v);
                sprintf_s(l, "[OLVR][DEVBRIDGE] %s trigger %.2f", h ? "right" : "left", v); OLProxyLog(l);
            }
        } else if (!_stricmp(cmd, "bones")) {
            OLUE3_DevBones();
        } else if (!_stricmp(cmd, "dgrip")) {
            char side[8] = { 0 }; float v = 0;
            if (sscanf_s(a, "%7s %f", side, (unsigned)sizeof(side), &v) >= 2) {
                const int h = (side[0] == 'R' || side[0] == 'r') ? 1 : 0;
                OLInteract_DevGripOverride(h, v);
                sprintf_s(l, "[OLVR][DEVBRIDGE] %s door grip forced to %.2f", h ? "right" : "left", v); OLProxyLog(l);
            }
        } else if (!_stricmp(cmd, "curl")) {
            float v[6] = { 0, 18, 22, 38, 48, 32 };
            sscanf_s(a, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
            OLUE3_DevCurl(v[0], v[1], v[2], v[3], v[4], v[5]);
            sprintf_s(l, "[OLVR][DEVBRIDGE] door curl thumb %.0f %.0f %.0f fingers %.0f %.0f %.0f", v[0], v[1], v[2], v[3], v[4], v[5]); OLProxyLog(l);
        } else if (!_stricmp(cmd, "headoff")) {
            float x = 0, y = 0, z = 0; sscanf_s(a, "%f %f %f", &x, &y, &z); OLXR_DevHeadOffset(x, y, z);
            sprintf_s(l, "[OLVR][DEVBRIDGE] head offset (%.2f %.2f %.2f) m", x, y, z); OLProxyLog(l);
        } else if (!_stricmp(cmd, "gripdump")) {
            OLUE3_DevGripDump();
        } else if (!_stricmp(cmd, "vrstick")) {                // (Test46) lx ly rx ry ms - the Quest sticks
            float v[4] = { 0 }; int ms = 300;
            if (sscanf_s(a, "%f %f %f %f %d", &v[0], &v[1], &v[2], &v[3], &ms) >= 4) OLXR_DevStick(v[0], v[1], v[2], v[3], ms);
        } else if (!_stricmp(cmd, "lensoff")) {                // (Test46) the lens in the camcorder's frame, uu
            float x = 0, y = 0, z = 0; if (sscanf_s(a, "%f %f %f", &x, &y, &z) == 3) OLUE3_DevLensOff(x, y, z);
        } else if (!_stricmp(cmd, "lstarget")) {
            OLUE3_DevLiveScreenTarget(a);
        } else if (!_stricmp(cmd, "lsres")) {
            OLUE3_DevLiveScreenOp(18, "", "");
        } else if (!_stricmp(cmd, "lstexlist")) {
            OLUE3_DevLiveScreenOp(17, a, "");
        } else if (!_stricmp(cmd, "lsscalar") || !_stricmp(cmd, "lsinfo")) {   // lsscalar <name> <value> | lsinfo <material>
            char s1[96] = "", s2[32] = ""; sscanf_s(a, "%95s %31s", s1, (unsigned)sizeof(s1), s2, (unsigned)sizeof(s2));
            OLUE3_DevLiveScreenOp(!_stricmp(cmd, "lsscalar") ? 15 : 16, s1, s2);
        } else if (!_stricmp(cmd, "lscap")) {                  // lscap <offset hex> <byte>
            char s1[32] = "", s2[32] = ""; sscanf_s(a, "%31s %31s", s1, (unsigned)sizeof(s1), s2, (unsigned)sizeof(s2)); OLUE3_DevLiveScreenOp(14, s1, s2);
        } else if (!_stricmp(cmd, "lscreate")) {               // lscreate <parent|orig> [slot]
            char s1[96] = "", s2[96] = ""; sscanf_s(a, "%95s %95s", s1, (unsigned)sizeof(s1), s2, (unsigned)sizeof(s2)); OLUE3_DevLiveScreenOp(13, s1, s2);
        } else if (!_stricmp(cmd, "lsfind")) {
            OLUE3_DevLiveScreenOp(12, "", "");
        } else if (!_stricmp(cmd, "lsmat")) {                  // (Test46) dump a material's emissive / diffuse inputs
            OLUE3_DevLiveScreenOp(11, a, "");
        } else if (!_stricmp(cmd, "lsparent")) {               // (Test46) orig | <material object name>
            OLUE3_DevLiveScreenOp(6, a, "");
        } else if (!_stricmp(cmd, "lsall") || !_stricmp(cmd, "lsone") || !_stricmp(cmd, "lsallorig") || !_stricmp(cmd, "lsoneorig")) {
            char s1[96] = "", s2[96] = "";
            const bool orig = strstr(cmd, "orig") != nullptr;
            if (!_strnicmp(cmd, "lsall", 5)) { sscanf_s(a, "%95s", s2, (unsigned)sizeof(s2)); OLUE3_DevLiveScreenOp(orig ? 9 : 7, "", s2); }
            else { sscanf_s(a, "%95s %95s", s1, (unsigned)sizeof(s1), s2, (unsigned)sizeof(s2)); OLUE3_DevLiveScreenOp(orig ? 10 : 8, s1, s2); }
        } else if (!_stricmp(cmd, "lstex")) {                  // (Test46) put a texture (or "rt") on the live screen
            OLUE3_DevLiveScreenTex(a);
        } else if (!_stricmp(cmd, "livescreen")) {             // (Test46) info | on | off
            OLUE3_DevLiveScreen(!_stricmp(a, "info") ? 1 : (!_stricmp(a, "on") ? 2 : (!_stricmp(a, "off") ? 3 : (!_stricmp(a, "mine") ? 4 : 1))));
        } else if (!_stricmp(cmd, "camtune")) {
            int w = 0; float x = 0, y = 0, z = 0;
            if (sscanf_s(a, "%d %f %f %f", &w, &x, &y, &z) == 4 && w >= 0 && w <= 5) OLUE3_DevCamTune(w, x, y, z);
        } else if (!_stricmp(cmd, "hide")) {
            unsigned long long addr = 0; int hv = 1;
            if (sscanf_s(a, "%llx %d", &addr, &hv) >= 1 && addr > 0x10000) OLUE3_DevQueueHide((void*)(uintptr_t)addr, hv);
        } else if (!_stricmp(cmd, "latch")) {
            OLXR_DevSetLatch(atoi(a));
            sprintf_s(l, "[OLVR][DEVBRIDGE] frame pose latch %s", atoi(a) ? "ON" : "OFF"); OLProxyLog(l);
        } else if (!_stricmp(cmd, "mark")) {
            sprintf_s(l, "[OLVR][DEVBRIDGE] ===== MARK: %s =====", a); OLProxyLog(l);
        }
    }
}
