"""Run actual HUD formatting, snapshot and movie integration logic with synthetic engine/XR APIs.
python3 tests/camcorder_hud_test.py (g++). Does not replace Windows/game/VR testing.
"""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
ue = (ROOT / 'src/ol_ue3.cpp').read_text()
interact = (ROOT / 'src/ol_interact.cpp').read_text()
reflection = ue[ue.index('static bool SP_FindPropIn(void* c, const char* want, unsigned* off, unsigned* mask) {'):ue.index('// -1 = unknown (not in gameplay yet') ]
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
int OLUE3_FindProp(const char*,const char* name,unsigned* off,unsigned* mask);

void* OLUE3_PlayerController() { return pc; }
void* ProbeReadPtr(char* p) { void* v; memcpy(&v,p,8); return v; }
bool BP_Read(char* p,void* out,size_t n) { memcpy(out,p,n); return true; }
const unsigned g_bpUNameOff = 0x40;
alignas(8) char hudClass[256]={}, visibleProp[256]={}, clockProp[256]={};
bool BP_Name(unsigned idx,char* out,size_t size) {
    std::snprintf(out,size,"%s",idx==1 ? "bVisible" : "RecordingTimeSeconds"); return true;
}
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
bool nvOn=false; extern "C" int OLProxy_NightVisionActive(){return nvOn?1:0;}
int s_camUp=1, s_numBat=10;
float s_energy=.5f, s_zoom=.75f;
void setup() {
    auto put32=[](char* p,unsigned off,unsigned value){memcpy(p+off,&value,4);};
    void* child=visibleProp; memcpy(hudClass+0x80,&child,8);
    child=clockProp; memcpy(visibleProp+0x60,&child,8);
    put32(visibleProp,g_bpUNameOff,1); put32(visibleProp,0x8C,40);
    put32(visibleProp,0x88,1); put32(visibleProp,0xC0,1);
    put32(clockProp,g_bpUNameOff,2); put32(clockProp,0x8C,24);
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
    unsigned off=0,mask=0;
    assert(SP_FindPropIn(hudClass,"bVisible",&off,nullptr) && off==40);
    assert(!SP_FindPropIn(hudClass,"bVisible",&off,&mask)); // actual singleton-bool rejection

    CamcorderHudTick(true); assert(OLInteract_CamcorderHud(&s));
    assert(s.recordingSeconds==3661.25f && s.spares==10 && s.capacity==10);
    assert(!s.nightVision);                                  // night vision off: no tell-tales
    nvOn=true; CamcorderHudTick(true);
    assert(OLInteract_CamcorderHud(&s) && s.nightVision);     // ...on, and the snapshot carries it
    nvOn=false; CamcorderHudTick(true);
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
    // Same no-op blit, but it remembers the ink each string was drawn in, so the palette itself can be
    // asserted (the real glyph raster is a Windows GDI concern, the colour is this file's).
    static std::map<std::string,std::string> ink;
    auto inkText=[](OLCamHud::Canvas&,int,int,int,bool,const char* s,uint8_t r,uint8_t g,uint8_t b){
        ink[s]=std::to_string(r)+"/"+std::to_string(g)+"/"+std::to_string(b);
    };
    OLCamHud::Canvas canvas;
    OLCamHud::Static(canvas,noText);
    assert(canvas.pixels[(500*1920+960)*4+3]==0); // transparent scene, no black screenshot background
    // [CAMFOCUS] The focusing brackets are NOT in the display's texture any more: they ride a quad of
    // their own, deeper than the display, so the middle of the display is empty and the box has its
    // own small raster. The display must not carry a second copy of it.
    assert(canvas.pixels[(481*1920+877)*4+3]==0);
    assert(canvas.pixels[(550*1920+877)*4+3]==0);
    assert(canvas.pixels[(550*1920+960)*4+3]==0);
    // HD badge: the perforations are transparent HOLEs, not black squares - the layer sits over the
    // scene, so a black square would be paint on the picture.
    assert(canvas.pixels[(825*1920+158)*4+3]==0);    // inside the first perforation
    assert(canvas.pixels[(836*1920+158)*4+3]==255);  // ...and the plate between them
    assert(canvas.pixels[(836*1920+158)*4+2]==128);  // #808079, like every other HUD icon
    assert(canvas.pixels[(825*1920+245)*4+3]==0);    // the right column is a hole too
    // The focus layer's own texture: 169x141 (the 167x139 box plus a transparent pixel), the box
    // drawn at (FocusPad, FocusPad), opaque, one pixel thin, grey #808079, hollow in the middle,
    // transparent everywhere else. Translucency is still not an option: the compositor multiplies the
    // layer by its own alpha despite XR_COMPOSITION_LAYER_UNPREMULTIPLIED and a 200/255 (and even a
    // 235/255) grey box came out black.
    OLCamHud::FocusCanvas focus;
    OLCamHud::Focus(focus);
    auto fx=[&](int x,int y,int ch){ return focus.pixels[(y*OLCamHud::FocusWidth+x)*4+ch]; };
    assert(fx(1,1,3)==255);                          // opaque: the blend is a plain copy
    assert(fx(1,1,2)==128 && fx(1,1,1)==128 && fx(1,1,0)==121);   // #808079, not the old bright 175
    assert(fx(10,2,3)==0);                           // one pixel thin: the bar is not two rows tall
    assert(fx(2,24,3)==0);                           // ...nor two columns wide
    assert(fx(1,24,3)==255);                         // the vertical arm is still there
    assert(fx(1,OLCamHud::FocusPad+OLCamHud::BracketH-1,3)==255); // bottom arm, last pixel of the box
    assert(fx(OLCamHud::FocusPad+OLCamHud::BracketW-1, OLCamHud::FocusPad+OLCamHud::BracketH-1, 3)==255);
    assert(fx(OLCamHud::FocusPad+OLCamHud::BracketW-1,24,3)==255); // right arm
    assert(fx(OLCamHud::FocusPad+OLCamHud::BracketArm,1,3)==0);    // gap between the top arms
    assert(fx(OLCamHud::FocusPad+OLCamHud::BracketW-OLCamHud::BracketArm,1,3)==255);
    assert(fx(84,70,3)==0);                          // hollow in the middle
    assert(fx(0,0,3)==0 && fx(84,0,3)==0 && fx(84,OLCamHud::FocusHeight-1,3)==0); // the pad is clear
    s.energy=1; s.zoom=0; s.nightVision=false; OLCamHud::Top(canvas,s,inkText);
    // The palette, on the strings themselves: the REC caption and the clock are the recording red,
    // everything else - the spare count included - is the HUD grey.
    assert(ink["REC"]=="167/16/0");
    assert(ink["01:01:01:25"]=="167/16/0");
    assert(ink["2 / 2"]=="128/128/121");
    assert(ink["W"]=="128/128/121" && ink["T"]=="128/128/121");
    assert(canvas.pixels[(75*1920+1836)*4+3]==255);  // full charge reaches the right end
    assert(canvas.pixels[(75*1920+1650)*4+3]==255);  // ...and the left end, next to the contact
    // The readouts were pushed out toward the corners and up; their old coordinates are empty now.
    assert(canvas.pixels[(80*1920+55)*4+2]==167 && canvas.pixels[(80*1920+55)*4+1]==16
        && canvas.pixels[(80*1920+55)*4+0]==0);      // REC lamp in #A71000
    assert(canvas.pixels[(100*1920+100)*4+3]==0);
    assert(canvas.pixels[(100*1920+1599)*4+3]==0);   // the gauge no longer starts at the old x
    // The gauge keeps its 212x48 cell in the top-right corner; the outline, the terminal and the
    // charge block are the reference's own proportions.
    assert(canvas.pixels[(75*1920+1850)*4+3]==0);    // right of the frame
    assert(canvas.pixels[(40*1920+1750)*4+3]==0);    // above it
    assert(canvas.pixels[(75*1920+1677)*4+3]==255);  // the bar
    assert(canvas.pixels[(75*1920+1677)*4+2]==132);  // ...with the reference's soft fall (grey+4 at row 14)
    assert(canvas.pixels[(75*1920+1845)*4+3]==255);  // the grey frame itself
    assert(canvas.pixels[(75*1920+1845)*4+2]==128);
    assert(canvas.pixels[(75*1920+1641)*4+3]==0);    // ...and its window is transparent
    // Contact on the left: a compact tab, 13x22 centred on the cell, whose 8x14 slot dead-ends at the
    // outline - cap and arms solid, slot transparent, and nothing above the tab.
    assert(canvas.pixels[(75*1920+1625)*4+3]==255);  // the tab's cap
    assert(canvas.pixels[(75*1920+1631)*4+3]==0);    // the slot
    assert(canvas.pixels[(67*1920+1630)*4+3]==255);  // the top arm, above the slot
    assert(canvas.pixels[(58*1920+1630)*4+3]==0);    // ...and the cell is empty above the tab
    // The charge DRAINS LEFT TO RIGHT: at half charge the bar that is left sits against the far end,
    // and the contact end is already empty.
    s.energy=.5f; OLCamHud::Top(canvas,s,noText);
    assert(canvas.pixels[(75*1920+1700)*4+3]==0);    // empty at the contact end ...
    assert(canvas.pixels[(75*1920+1800)*4+3]==255);  // ... filled toward the right
    assert(canvas.pixels[(75*1920+1800)*4+2]==132 && canvas.pixels[(75*1920+1800)*4+0]==125);
    // Low charge keeps its warning, now in the recording red.
    s.energy=.25f; OLCamHud::Top(canvas,s,noText);
    assert(canvas.pixels[(75*1920+1800)*4+2]==167 && canvas.pixels[(75*1920+1800)*4+1]==16);
    // Night-vision tell-tales: gone while night vision is off...
    assert(canvas.pixels[(106*1920+1800)*4+3]==0);
    s.energy=.5f; s.nightVision=true; OLCamHud::Top(canvas,s,noText);
    assert(canvas.pixels[(106*1920+1800)*4+3]==255);  // the speaker's frame
    assert(canvas.pixels[(106*1920+1730)*4+3]==255);  // the eye's frame
    assert(canvas.pixels[(110*1920+1720)*4+3]==0);    // hollow inside
    assert(canvas.pixels[(106*1920+1800)*4+2]==128);  // #808079, right-aligned under the gauge
    s.nightVision=false; OLCamHud::Top(canvas,s,noText);
    assert(canvas.pixels[(106*1920+1800)*4+3]==0);    // switched off, and the band clears them
    // The W-T thumb follows the zoom factor instead of sitting at one end of the lever.
    s.zoom=0; OLCamHud::Top(canvas,s,noText);
    assert(canvas.pixels[(80*1920+820)*4+1]>=120);            // thumb at the W end
    s.zoom=.5f; OLCamHud::Top(canvas,s,noText);
    assert(canvas.pixels[(80*1920+820)*4+1]<90);              // ...and gone from it
    assert(canvas.pixels[(80*1920+950)*4+1]>=120);            // ...sitting in the middle
    s.zoom=1; OLCamHud::Top(canvas,s,noText);
    assert(canvas.pixels[(80*1920+950)*4+1]<90);
    assert(canvas.pixels[(75*1920+1076)*4+3]==255);           // T endpoint
    s.energy=0; OLCamHud::Top(canvas,s,noText);
    assert(canvas.pixels[(75*1920+1700)*4+3]==0); // empty battery actually empty
    assert(canvas.pixels[(75*1920+1836)*4+3]==0);
    assert(canvas.pixels[(462*1920+855)*4+3]==0); // dynamic updates preserve nothing of the old box
    assert(canvas.pixels[(481*1920+877)*4+3]==0); // ...and the display carries no brackets at all
    assert(fx(1,1,3)==255);                       // (they live in the focus layer's own texture)
    assert(OLCamHud::Unit(std::numeric_limits<float>::infinity())==0);
    // Fullscreen scale: the display has to span the view, not sit as a panel in the middle of it.
    // The width follows PanelDistance (2 m now), so the same fill is a wider plane than it was at 1 m.
    assert(OLCamHud::PanelWidth(100.0f) > 2.55f && OLCamHud::PanelWidth(100.0f) < 2.70f);
    // ...and it stops short of the lens edge, which is what sets the fill (PanelWidth scales with
    // PanelDistance, so the clamp, not the parking distance, is what puts the readouts at ~32 deg).
    auto halfDeg = [](float width){ return float(std::atan(0.5f*width/OLCamHud::PanelDistance)*57.29578f); };
    assert(halfDeg(OLCamHud::PanelWidth(100.0f)) > 28.0f);   // still spans the view
    assert(halfDeg(OLCamHud::PanelWidth(100.0f)) < 35.0f);   // set back, clear of the lens edge
    // A quad layer in the VIEW space is composited per eye, so PanelDistance IS the convergence the
    // display asks the eyes for. At 1.05 m that demand was 3.4 deg - more than any room - and the
    // thin centre brackets could not fuse against the scene, which read as a doubled square. Keep
    // it near a room's own convergence; pulling it back under ~1.6 m (over 2.2 deg) is the old
    // doubled-square report coming back.
    const float kHalfIpd = 0.0315f;                          // a textbook 63 mm IPD
    const float convDeg = 2.0f * std::atan(kHalfIpd / OLCamHud::PanelDistance) * 57.29578f;
    assert(convDeg > 1.5f && convDeg < 2.2f);
    assert(OLCamHud::PanelWidth(84.0f) == OLCamHud::PanelWidth(180.0f)); // clamped to the same fill
    assert(OLCamHud::PanelWidth(0.0f) > 1.0f);       // no runtime FOV yet: sane floor
    assert(OLCamHud::PanelWidth(-5.0f) > 1.0f);
    assert(OLCamHud::ZoomUnit(std::numeric_limits<float>::quiet_NaN())==0);
    assert(OLCamHud::ZoomUnit(-1.0f)==0 && OLCamHud::ZoomUnit(4000.0f)==1);
    assert(std::fabs(OLCamHud::ZoomUnit(0.4f)-0.4f) < 1e-6f);
    assert(std::fabs(OLCamHud::ZoomUnit(40.0f)-0.4f) < 1e-6f);   // a 0..100 build still moves
    // [CAMFOCUS] The focusing brackets ride a quad of their own, FocusDistance out. The quad is scaled
    // and offset by FocusDistance/PanelDistance, which is what keeps the box on exactly the same pixels
    // of the view: same place, same apparent size, more depth. Anything else would move the one shape
    // the readouts were laid out around.
    assert(OLCamHud::FocusDistance > OLCamHud::PanelDistance);
    const float pw = OLCamHud::PanelWidth(96.0f);
    const float ps = OLCamHud::FocusDistance / OLCamHud::PanelDistance;
    const float pt = pw / float(OLCamHud::Width);                 // metres per display pixel
    const OLCamHud::FocusQuadSize fq = OLCamHud::FocusQuadFor(pw);
    assert(std::fabs(fq.width  - float(OLCamHud::FocusWidth ) * pt * ps) < 1e-5f);
    assert(std::fabs(fq.height - float(OLCamHud::FocusHeight) * pt * ps) < 1e-5f);
    assert(std::fabs(fq.offsetX - (OLCamHud::BracketX + 0.5f*OLCamHud::BracketW - 0.5f*OLCamHud::Width) * pt * ps) < 1e-5f);
    assert(std::fabs(fq.offsetY - (0.5f*OLCamHud::Height - (OLCamHud::BracketY + 0.5f*OLCamHud::BracketH)) * pt * ps) < 1e-5f);
    assert(std::fabs(fq.offsetX) < 0.01f && fq.offsetY < 0.0f);   // dead centre, a hair below it
    // The same angle the box subtended on the display: 2*atan(half/far) is unchanged by the move.
    const float boxDegPanel = 2.0f * std::atan(0.5f*(float(OLCamHud::FocusWidth)*pt)/OLCamHud::PanelDistance) * 57.29578f;
    const float boxDegFocus = 2.0f * std::atan(0.5f*fq.width/OLCamHud::FocusDistance) * 57.29578f;
    assert(std::fabs(boxDegPanel - boxDegFocus) < 1e-3f);
    // ...and it asks for well under the display's convergence (FocusDistance = 8 m: ~0.45 deg), so a
    // deep box is comfortable to look at while the readouts stay on their own nearer plane.
    const float focusConvDeg = 2.0f * std::atan(kHalfIpd / OLCamHud::FocusDistance) * 57.29578f;
    assert(focusConvDeg > 0.3f && focusConvDeg < 1.2f);
    assert(focusConvDeg < convDeg);
    assert(OLCamHud::FocusQuadFor(0.0f).width == 0.0f);          // no panel width: no quad, no NaN
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source=Path(tmp)/'hud.cpp'; exe=Path(tmp)/'hud-test'
    source.write_text(stubs+reflection+r'''
int OLUE3_FindProp(const char*,const char* name,unsigned* off,unsigned* mask) {
    if(std::string(name)=="bVisible") return SP_FindPropIn(hudClass,name,off,mask);
    *off=offsets[name]; if(mask)*mask=1; return *off!=0;
}
'''+engine+snapshot+checks)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT/'src'),str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Camcorder HUD: clock, data, lifecycle, fallback and transparent artwork checks passed.')
