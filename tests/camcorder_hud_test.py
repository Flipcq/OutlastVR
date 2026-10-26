"""Run actual HUD formatting, snapshot and movie integration logic with synthetic engine/XR APIs.
python3 tests/camcorder_hud_test.py (g++). Does not replace Windows/game/VR testing.
"""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
ue = (ROOT / 'src/ol_ue3.cpp').read_text()
interact = (ROOT / 'src/ol_interact.cpp').read_text()
engine = ue[ue.index('extern "C" int OLUE3_CamcorderHudRead('):ue.index("// a function's parameters, for the log")]
snapshot = interact[interact.index('// [CAMHUD] XR and gameplay'):interact.index('static bool  s_gripDown')]
stubs = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
// Windows.h exposes these macros in the production translation units.
#define max(a,b) windows_max_macro_must_not_expand(a,b)
#define min(a,b) windows_min_macro_must_not_expand(a,b)
#include "ol_camcorder_hud.h"
#undef max
#undef min
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <limits>
using ULONGLONG = unsigned long long;
using LONG64 = long long;
using SRWLOCK = int;
#define SRWLOCK_INIT 0
void AcquireSRWLockShared(int*) {} void ReleaseSRWLockShared(int*) {}
void AcquireSRWLockExclusive(int*) {} void ReleaseSRWLockExclusive(int*) {}
LONG64 InterlockedExchange64(volatile LONG64* p, LONG64 v) { auto old=*p; *p=v; return old; }
LONG64 InterlockedCompareExchange64(volatile LONG64* p, LONG64 v, LONG64 c) { auto old=*p; if(old==c)*p=v; return old; }
ULONGLONG now = 4000;
ULONGLONG GetTickCount64() { return now; }
alignas(8) char pc[128]={}, hud[128]={}, movie[128]={}, replacement[128]={};
static std::map<std::string,unsigned> offsets = {{"myHUD",8},{"CamcorderHUD",16},{"RecordingTimeSeconds",24},{"MaxNumBatteries",32},{"bVisible",40}};
int OLUE3_FindProp(const char*,const char* name,unsigned* off,unsigned* mask) {
    *off=offsets[name]; if(mask)*mask=1; return *off!=0;
}
void* OLUE3_PlayerController() { return pc; }
void* ProbeReadPtr(char* p) { void* v; memcpy(&v,p,8); return v; }
bool BP_Read(char* p,void* out,size_t n) { memcpy(out,p,n); return true; }
void OLProxyLog(const char*) {}
void* LS_FindFunction(const char*,const char* name) { return reinterpret_cast<void*>(uintptr_t(std::string(name)=="GetVariableNumber" ? 1 : 2)); }
int LS_ParamOff(void*,const char* name) { return std::string(name)=="Path" ? 0 : 16; }
float alpha = 85;
bool callOk=true, menu=false, modMenu=false;
bool LS_Call(void*,void* fn,void* params) {
    if(!callOk)return false;
    assert(std::wstring(*(wchar_t**)params)==L"_root._alpha");
    if(fn==(void*)1)memcpy((char*)params+16,&alpha,4);
    else memcpy(&alpha,(char*)params+16,4);
    return true;
}
int OLUE3_InGameMenu(){return menu;} int OLMenu_Visible(){return modMenu;}
long g_camHud=1;
int s_camUp=1, s_numBat=10;
float s_energy=.5f, s_zoom=.75f;
void setup() {
    void* h=hud; memcpy(pc+8,&h,8); void* m=movie; memcpy(hud+16,&m,8);
    float t=3661.25f; memcpy(movie+24,&t,4); int max=10; memcpy(pc+32,&max,4);
    unsigned visible=1; memcpy(movie+40,&visible,4);
}
'''
checks = r'''
int main() {
    char t[12]; OLCamHud::Timecode(3661.25f,t); assert(std::string(t)=="01:01:01:25");
    OLCamHud::Timecode(60,t); assert(std::string(t)=="00:01:00:00");
    OLCamHud::Timecode(360000,t); assert(std::string(t)=="00:00:00:00");
    OLCamHud::Timecode(std::numeric_limits<float>::quiet_NaN(),t); assert(std::string(t)=="00:00:00:00");
    setup(); OLCamcorderHudState s;
    CamcorderHudTick(true); assert(OLInteract_CamcorderHud(&s));
    assert(s.recordingSeconds==3661.25f && s.spares==10 && s.capacity==10);
    assert(s.energy==.5f && s.zoom==.75f && alpha==85); // retain native display until XR succeeds
    OLInteract_CamcorderHudSubmitted(1); CamcorderHudTick(true); assert(alpha==0);
    now+=100; CamcorderHudTick(true); assert(OLInteract_CamcorderHud(&s) && s.recordingSeconds==3661.25f);
    menu=true; CamcorderHudTick(true); assert(!OLInteract_CamcorderHud(&s) && alpha==85);
    menu=false; CamcorderHudTick(true); assert(alpha==0);
    OLInteract_CamcorderHudSubmitted(0); CamcorderHudTick(true); assert(alpha==85);
    OLInteract_CamcorderHudSubmitted(1); CamcorderHudTick(true); assert(alpha==0);
    OLInteract_SetCamcorderHud(0); CamcorderHudTick(true); assert(alpha==85 && !OLInteract_CamcorderHud(&s));
    OLInteract_SetCamcorderHud(1); CamcorderHudTick(true); now+=501; CamcorderHudTick(true); assert(alpha==85);
    now+=1001; assert(!OLInteract_CamcorderHud(&s)); // no stale HUD during gameplay teardown
    now+=1; s_camUp=0; CamcorderHudTick(true); assert(!OLInteract_CamcorderHud(&s));
    s_camUp=1; int hard=2; memcpy(pc+32,&hard,4); s_numBat=2;
    CamcorderHudTick(true); assert(OLInteract_CamcorderHud(&s) && s.capacity==2 && s.spares==2);
    callOk=false; OLInteract_CamcorderHudSubmitted(1); CamcorderHudTick(true);
    assert(!OLInteract_CamcorderHud(&s) && alpha==85);
    callOk=true; CamcorderHudTick(true); assert(alpha==0);
    CamcorderHudTick(false); assert(alpha==85 && !OLInteract_CamcorderHud(&s));
    auto noText=[](OLCamHud::Canvas&,int,int,int,bool,const char*,uint8_t,uint8_t,uint8_t){};
    OLCamHud::Canvas canvas;
    OLCamHud::Static(canvas,noText);
    assert(canvas.pixels[(500*1920+960)*4+3]==0); // transparent scene, no black screenshot background
    assert(canvas.pixels[(402*1920+784)*4+3]==255);
    s.energy=1; s.zoom=0; OLCamHud::Top(canvas,s,noText);
    assert(canvas.pixels[(100*1920+1764)*4+3]==255); // full charge reaches right edge
    s.energy=.25f; OLCamHud::Top(canvas,s,noText);
    assert(canvas.pixels[(100*1920+1623)*4+2]==255 && canvas.pixels[(100*1920+1623)*4]==60);
    s.energy=.5f; OLCamHud::Top(canvas,s,noText);
    assert(canvas.pixels[(100*1920+1623)*4+2]==canvas.pixels[(100*1920+1623)*4]);
    s.energy=0; s.zoom=1; OLCamHud::Top(canvas,s,noText);
    assert(canvas.pixels[(100*1920+1623)*4+3]==0); // empty battery actually empty
    assert(canvas.pixels[(94*1920+1076)*4+3]==255); // T endpoint
    assert(canvas.pixels[(402*1920+784)*4+3]==255); // dynamic updates preserve reticle
    assert(OLCamHud::Unit(std::numeric_limits<float>::infinity())==0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source=Path(tmp)/'hud.cpp'; exe=Path(tmp)/'hud-test'
    source.write_text(stubs+engine+snapshot+checks)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT/'src'),str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Camcorder HUD: clock, data, lifecycle, fallback and transparent artwork checks passed.')
