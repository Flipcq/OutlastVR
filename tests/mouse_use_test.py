"""Compile actual mouse adapter, XR wiring, mapper, XInput hook and hint rewrite.
Windows/game APIs are synthetic: this does not prove native mouse acceptance.
Run: python3 tests/mouse_use_test.py (g++ required).
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
proxy = (ROOT / 'src/d3d9_proxy.cpp').read_text()
xr = (ROOT / 'src/ol_xr.cpp').read_text()
prompts = (ROOT / 'src/ol_prompts.cpp').read_text()
menu = (ROOT / 'src/ol_menu.cpp').read_text()
mouse_hook = menu[menu.index('static LRESULT CALLBACK LLMouseProc('):menu.index('static DWORD WINAPI InputThread(')]
adapter = proxy[proxy.index('// [MOUSEUSE] The requested'):proxy.index('// OpenXR and UE3/XInput are sampled')]
mapping = proxy[proxy.index('static WORD ReliablePadButtons('):proxy.index('// [HANDCAM] Right grip')]
hook = proxy[proxy.index('static DWORD WINAPI Hook_XiGetState('):proxy.index('// [HAPTICS] XInputSetState')]
wiring = xr[xr.index('    OLProxy_MouseUseSample(p.buttons'):xr.index('    AcquireSRWLockExclusive(&g_pubLock);', xr.index('    OLProxy_MouseUseSample(p.buttons'))]
rewrite = prompts[prompts.index('struct ActionWord'):prompts.index('// The first few distinct rewrites')]
interact = (ROOT / 'src/ol_interact.cpp').read_text()
drop = interact[interact.index('            const bool dropDown ='):interact.index(';', interact.index('            const bool dropDown =')) + 1]
STUBS = r'''
#include "ol_mouse_use.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <string>
#define WINAPI
#define _countof(a) (sizeof(a)/sizeof(a[0]))
#define ZeroMemory(p,n) memset(p,0,n)
#define _wcsnicmp wcsncasecmp
using WORD=unsigned short; using BYTE=unsigned char; using SHORT=short;
using DWORD=unsigned long; using LONG=long; using ULONGLONG=unsigned long long;
using XrResult=int;
static constexpr int XR_SUCCESS_VALUE=0, INPUT_MOUSE=0, MOUSEEVENTF_LEFTDOWN=2,
    MOUSEEVENTF_LEFTUP=4, VK_LBUTTON=1, ERROR_SUCCESS=0, ERROR_DEVICE_NOT_CONNECTED=1167;
static ULONGLONG now=1000;
static ULONGLONG GetTickCount64() { return now; }
static DWORD GetTickCount() { return DWORD(now); }
struct INPUT { unsigned type; struct { unsigned dwFlags; unsigned long dwExtraInfo; } mi; };
struct SRWLOCK { bool locked=false; };
#define SRWLOCK_INIT {}
static void AcquireSRWLockExclusive(SRWLOCK* l) { assert(!l->locked); l->locked=true; }
static void ReleaseSRWLockExclusive(SRWLOCK* l) { assert(l->locked); l->locked=false; }
static bool TryAcquireSRWLockExclusive(SRWLOCK* l) { if(l->locked)return false; l->locked=true; return true; }
static LONG InterlockedExchange(volatile LONG* p,LONG n) { LONG old=*p; *p=n; return old; }
static LONG InterlockedIncrement(volatile LONG* p) { return ++*p; }
static int g_gameWnd=1, foreground=1;
static int GetForegroundWindow() { return foreground; }
static bool modMenu=false, gameMenu=false, gameplay=true, physicalMouse=false;
static int OLMenu_Visible() { return modMenu; }
extern "C" int OLUE3_InGameMenu() { return gameMenu; }
extern "C" int OLUE3_GameplayActive() { return gameplay; }
static short GetAsyncKeyState(int) { return physicalMouse ? short(0x8000) : 0; }
static bool failDown=false, failUp=false;
static std::vector<bool> events;
static unsigned attempts=0;
static unsigned SendInput(unsigned count,INPUT* in,int size) {
    assert(count==1 && size==sizeof(INPUT) && in->type==INPUT_MOUSE);
    assert(in->mi.dwExtraInfo==OLMouseUse::InputTag);
    const bool down=in->mi.dwFlags==MOUSEEVENTF_LEFTDOWN;
    assert(down || in->mi.dwFlags==MOUSEEVENTF_LEFTUP); ++attempts;
    if(down ? failDown : failUp) return 0;
    events.push_back(down); return 1;
}
static std::vector<std::string> logs;
static void Log(const char* fmt,...) { char s[1024]; va_list ap;va_start(ap,fmt);vsnprintf(s,sizeof(s),fmt,ap);va_end(ap);logs.emplace_back(s); }
#define XLOG Log
static bool g_camcorderUp=false,g_nvRequested=false;
static constexpr ULONGLONG OLVR_CAM_HOLD_MS=800;
static LONG g_menuChordLatch=0;
static OLXrPad published={};
static bool motionReady=true,padReady=true;
extern "C" int OLXR_MotionControlsReady() { return motionReady; }
extern "C" int OLXR_GetPad(OLXrPad* p) { if(!padReady)return 0;*p=published;return 1; }
struct OLXiGamepad { WORD wButtons; BYTE bLeftTrigger,bRightTrigger; SHORT sThumbLX,sThumbLY,sThumbRX,sThumbRY; };
struct OLXiState { DWORD dwPacketNumber; OLXiGamepad Gamepad; };
static volatile LONG g_padPollsAll=0,g_padPolls0=0,g_padRealOk=0,g_padFaked=0;
static bool g_padStub=false,realConnected=false;
static WORD realButtons=0;
static DWORD RealPad(DWORD,OLXiState* p) { ZeroMemory(p,sizeof(*p));p->Gamepad.wButtons=realButtons;return realConnected?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED; }
static auto o_XiGetState=&RealPad;
struct OLInteractPad { int camUp; bool blockGripL,blockGripR; WORD addButtons; bool stickOn; float stickLX,stickLY; };
static int OLDev_Pad(WORD*,float*,float*,float*,float*) { return 0; }
static void OLInteract_Pad(const OLXrPad&,OLInteractPad* p) { p->camUp=-1; }
static WORD CamcorderGripButton(const OLXrPad&) { return 0; }
static WORD OLMotion_Process(const OLXrPad&,bool,float*,float*,float*,float*) { return 0; }
static void OLInteract_FilterMoveStick(float*,float*) {}
static SHORT StickAxis(float v) { return SHORT(v*32767); }
static int OLInteract_Get(int) { return 1; }
static int OLMotion_GetPhysCrouch() { return 1; }
'''
MENU_STUBS = r'''
#define CALLBACK
#define HIWORD(x) (((x)>>16)&0xffff)
using LRESULT=long;using WPARAM=unsigned;using LPARAM=intptr_t;
struct POINT { long x,y; };
struct MSLLHOOKSTRUCT { POINT pt; unsigned long mouseData, flags, dwExtraInfo; };
static constexpr int HC_ACTION=0,LLMHF_INJECTED=1,
 WM_MOUSEMOVE=0x200,WM_LBUTTONDOWN=0x201,WM_LBUTTONUP=0x202,
 WM_RBUTTONDOWN=0x204,WM_RBUTTONUP=0x205,WM_MOUSEWHEEL=0x20a,
 WM_MBUTTONDOWN=0x207,WM_MBUTTONUP=0x208;
static bool g_visible=false;
static int g_llHook=1;
static long g_mouseMoved=0,g_lBtn=0,g_rBtn=0,g_wheel=0;
static float g_curX=0,g_curY=0;
static constexpr int kRTW=100,kRTH=100;
static POINT g_center={};
static void SetCursorPos(long,long) {}
static void InterlockedExchangeAdd(volatile long* p,long v) { *p+=v; }
static LRESULT CallNextHookEx(int,int,WPARAM,LPARAM) { return 42; }
'''
WRAPPERS = r'''
static void Publish(unsigned buttons=0,float grip=0,int focus=1) {
    OLXrPad p={}; p.buttons=buttons;p.gripL=grip;
    const XrResult r=focus?0:8;
''' + wiring + r'''
    published=p;
}
static bool DropRequested() { OLXrPad pp={};
''' + drop + r'''
    return dropDown;
}
static WORD Poll() { OLXiState s={}; assert(Hook_XiGetState(0,&s)==ERROR_SUCCESS);return s.Gamepad.wButtons; }
static void Idle() { now+=300;Publish();Poll(); }
static void Hint(const wchar_t* text,const wchar_t* expected) {
    wchar_t out[2048]; assert(Rewrite(text,wcslen(text),out,2048)); assert(wcscmp(out,expected)==0);
}
'''
CASES = r'''
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    Idle(); events.clear();
    // Source tap entirely between two game polls: ordered mouse edges survive,
    // without a buffer tail or any fabricated gamepad Use/crouch for B.
    now+=11; Publish(OLXR_BTN_B); now+=11;Publish();
    assert((events==std::vector<bool>{true,false}));assert(Poll()==0);
    events.clear();now+=200;Publish(OLXR_BTN_B);assert(Poll()==0);
    now+=2000;Publish(OLXR_BTN_B);assert(Poll()==0 && events.size()==1);
    Publish();assert((events==std::vector<bool>{true,false}));
    puts("PASS: raw short/held B, no mouse tail, no duplicate gamepad action");

    Idle();events.clear();Publish(OLXR_BTN_X);assert(Poll()==0x2000 && DropRequested());
    now+=11;Publish();assert(Poll()==0x2000); // retain the base gamepad extension for crouch
    now+=100;Publish();assert(Poll()==0);assert(events.empty());
    Idle();Publish(OLXR_BTN_A|OLXR_BTN_Y);assert(Poll()==0x9000);
    // (Beta 1.1.4) left grip + X and left grip + B no longer open the notes / notebook menus; the
    // grip is still sprint, so those presses leave the run alone.
    Idle();Publish(OLXR_BTN_X,1);assert(Poll()==0x0100 && !DropRequested());
    Idle();Publish(OLXR_BTN_B,1);assert(Poll()==0x0100);
    Idle();Publish(OLXR_BTN_Y,1);assert(Poll()==0x0008);
    assert(events.empty());
    // Releasing the modifier under a held B never creates a mouse click.
    Publish(OLXR_BTN_B,1);Publish(OLXR_BTN_B,0);assert(events.empty());
    Idle();Publish(OLXR_BTN_B);assert(events.size()==1);Publish(OLXR_BTN_B,1);
    assert((events==std::vector<bool>{true,false}));Publish(OLXR_BTN_B,0);assert(events.size()==2);
    puts("PASS: X crouch, original gamepad timing, A/Y and grip+Y only (grip+X/B removed)");

    Idle();events.clear();gameMenu=true;Publish();Poll();
    Publish(OLXR_BTN_B);assert(Poll()==0x2000);assert(events.empty());
    gameMenu=false;Publish(OLXR_BTN_B);assert(Poll()==0 && events.empty());
    Idle();gameMenu=true;Publish();Poll();Publish(OLXR_BTN_X);assert(Poll()==0x4000);
    gameMenu=false;Publish(OLXR_BTN_X);assert(Poll()==0);
    now+=200;Publish(OLXR_BTN_X);assert(Poll()==0);Idle();
    Publish(OLXR_BTN_X);assert(Poll()==0x2000);
    gameMenu=true;Publish(OLXR_BTN_X);assert(Poll()==0); // no menu activation from held crouch
    Idle();gameMenu=false;Publish();Poll();
    modMenu=true;Publish(OLXR_BTN_B);assert(Poll()==0 && events.empty());
    modMenu=false;Publish(OLXR_BTN_B);assert(events.empty());Idle();
    modMenu=true;Publish(OLXR_BTN_X);assert(Poll()==0);
    modMenu=false;Publish(OLXR_BTN_X);assert(Poll()==0);Idle();
    puts("PASS: game-menu original B/X, mod menu, held-button context boundaries");

    // Focus loss and XR/dashboard loss release and require a fresh press.
    events.clear();Publish(OLXR_BTN_B);foreground=2;MouseUseWatchdog();
    assert((events==std::vector<bool>{true,false}));foreground=1;
    Publish(OLXR_BTN_B);assert(events.size()==2);Idle();
    Publish(OLXR_BTN_B);OLProxy_MouseUseSample(0,0,0);
    Publish(OLXR_BTN_B);assert(events.size()==4);Idle();
    Publish(OLXR_BTN_B);now+=201;MouseUseWatchdog();
    assert(events.size()==6);Publish(OLXR_BTN_B);assert(events.size()==6);Idle();
    Publish(OLXR_BTN_B);gameMenu=true;MouseUseWatchdog();assert(events.size()==8);
    gameMenu=false;Publish(OLXR_BTN_B);assert(events.size()==8);Idle();
    gameplay=false;Publish(OLXR_BTN_B);gameplay=true;Publish(OLXR_BTN_B);assert(events.size()==8);Idle();
    physicalMouse=true;Publish(OLXR_BTN_B);physicalMouse=false;Publish(OLXR_BTN_B);assert(events.size()==8);Idle();
    Publish(OLXR_BTN_B);gameplay=false;Publish(OLXR_BTN_B);assert(g_mouseUseState.held);
    Publish();gameplay=true;assert(events.size()==10); // preserve hold across door animation
    puts("PASS: focus, XR loss, stale producer, menus, native animation, physical mouse ownership");

    events.clear();failDown=true;Publish(OLXR_BTN_B);assert(!g_mouseUseState.held);
    failDown=false;Publish(OLXR_BTN_B);assert(events.empty());Idle();
    Publish(OLXR_BTN_B);failUp=true;Publish();assert(g_mouseUseState.held);
    failUp=false;Publish(OLXR_BTN_B);assert(!g_mouseUseState.held); // retries pending up even under a new press
    assert((events==std::vector<bool>{true,false}));Idle();
    Publish(OLXR_BTN_B);failUp=true;Publish();failUp=false;MouseUseWatchdog();assert(!g_mouseUseState.held);Idle();
    puts("PASS: failed down is not replayed, failed up ownership and retry");

    // Original pad bits are additive, independent of the new VR layout.
    events.clear();realConnected=true;realButtons=0x4000;
    Publish(OLXR_BTN_B);assert(Poll()==0x4000);Publish();
    assert((events==std::vector<bool>{true,false}));realConnected=false;realButtons=0;Idle();
    g_mouseUse=0;events.clear();Publish(OLXR_BTN_X);assert(Poll()==0x4000);
    Idle();Publish(OLXR_BTN_B);assert(Poll()==0x2000 && events.empty() && DropRequested());Idle();
    puts("PASS: real gamepad preserved and MouseUse=0 legacy layout");

    g_mouseUse=1;
    Hint(L"Pressione {OLA_Use} para fechar",L"Pressione (B) para fechar");
    Hint(L"Press {OLA_CrouchToggle}",L"Press X");
    Hint(L"Press {OLA_Crouch}",L"Press X");
    Hint(L"Press {OLA_ShowTabMenu}",L"Press no shortcut");
    Hint(L"Press {OLA_ShowRecordingMenu}",L"Press no shortcut");
    Hint(L"Press {OLA_Use} to close",L"Grab the handle to close, or press B");
    gameMenu=true;Hint(L"Press {OLA_Use}",L"Press X");Hint(L"Press {OLA_CrouchToggle}",L"Press B");
    gameMenu=false;g_mouseUse=0;Hint(L"Press {OLA_Use} to close",L"Grab the handle to close, or press X");
    puts("PASS: localized Use/crouch hints, English door hints, shortcuts and menu/legacy text");

    // Actual settings-menu low-level hook must pass our cancellation release,
    // while original physical and other injected clicks still drive its cursor.
    g_visible=true;MSLLHOOKSTRUCT event={};event.flags=LLMHF_INJECTED;
    event.dwExtraInfo=OLMouseUse::InputTag;
    assert(LLMouseProc(HC_ACTION,WM_LBUTTONUP,reinterpret_cast<LPARAM>(&event))==42);
    assert(g_lBtn==0);
    event.dwExtraInfo=0;
    assert(LLMouseProc(HC_ACTION,WM_LBUTTONDOWN,reinterpret_cast<LPARAM>(&event))==1 && g_lBtn==1);
    event.flags=0;
    assert(LLMouseProc(HC_ACTION,WM_LBUTTONUP,reinterpret_cast<LPARAM>(&event))==1 && g_lBtn==0);
    g_visible=false;
    puts("PASS: tagged owned releases pass the real menu hook; original menu mouse controls preserved");

    g_mouseUse=1;Idle();Publish(OLXR_BTN_B);const auto n=logs.size();
    OLProxy_MouseUseShutdown();assert(!g_mouseUseHeld && !g_mouseUseState.held && logs.size()==n);
    Publish(OLXR_BTN_B);assert(!g_mouseUseState.held);
    // Busy worker during shutdown: the loader-lock path does not wait or log.
    g_mouseUseStopping=0;Idle();Publish(OLXR_BTN_B);AcquireSRWLockExclusive(&g_mouseUseLock);
    OLProxy_MouseUseShutdown();assert(!g_mouseUseHeld);ReleaseSRWLockExclusive(&g_mouseUseLock);
    Publish();assert(!g_mouseUseState.held);
    puts("PASS: shutdown release, stopped input, nonblocking shutdown fallback");
}
'''
# Compile the actual raw source call before publication, without the previous
# experimental source buffer or diagnostics.
assert wiring.strip() == 'OLProxy_MouseUseSample(p.buttons, p.gripL, r == XR_SUCCESS_VALUE);'
assert 'MapPadButtons(pg, inMenu)' in hook
assert 'MouseUseWatchdog();' in proxy[proxy.index('static DWORD WINAPI KeyThread'):]
assert 'OLProxy_MouseUseShutdown();\n        OLXR_Shutdown();' in proxy
with tempfile.TemporaryDirectory(prefix='outlast-mouse-use-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    binary = Path(tmp) / 'test'
    cpp.write_text(STUBS + adapter + mapping + hook + rewrite + MENU_STUBS + mouse_hook + WRAPPERS + CASES)
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'src'), str(cpp), '-o', str(binary)], check=True)
    raise SystemExit(subprocess.run([str(binary)]).returncode)
