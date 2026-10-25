"""Run the production door loop with synthetic door memory on a host C++ compiler.

    python3 tests/door_native_test.py
    python3 tests/door_native_test.py --source /tmp/pre-fix-ol_interact.cpp

Only Windows/game APIs are stubbed; ReadDoor, ownership and hand physics are
extracted unchanged from the selected source. No game files are needed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, default=ROOT / "src/ol_interact.cpp")
args = parser.parse_args()
source = args.source.read_text()
door_code = source[source.index("struct NearDoor {"):source.index("// ---- climbing (Test40:")]

STUBS = r'''
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#define __try try
#define __except(...) catch (...)
using ULONGLONG = uint64_t;
struct V3 { float x, y, z; };
static V3 Sub(V3 a, V3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static float Len(V3 a) { return sqrtf(a.x*a.x+a.y*a.y+a.z*a.z); }
static float Wrap180(float a) { while(a>180) a-=360; while(a< -180) a+=360; return a; }
struct FakeDoor {
    int pad; V3 loc; float ratio, target, maxAngle;
    unsigned bits; unsigned char type, state, broken; void* user;
};
static FakeDoor door, hero;
static struct {
    unsigned dtype=offsetof(FakeDoor,type), bits=offsetof(FakeDoor,bits),
      openR=offsetof(FakeDoor,ratio), tgtR=offsetof(FakeDoor,target),
      maxAng=offsetof(FakeDoor,maxAngle), dstate=offsetof(FakeDoor,state),
      dbreak=offsetof(FakeDoor,broken), duser=offsetof(FakeDoor,user), loc=offsetof(FakeDoor,loc),
      mLocked=1, mFake=2, mRev=4, mCant=8, mBusyBits=16, cyl=0, cylR=0;
} F;
template<class T> static bool Rd(const void* p,unsigned off,T* out) {
    if(!p || !off) return false;
    memcpy(out,static_cast<const char*>(p)+off,sizeof(T)); return true;
}
static int writes=0;
static bool WrF(void* p,unsigned off,float v) {
    ++writes; if(!p || !off) return false;
    memcpy(static_cast<char*>(p)+off,&v,sizeof(v)); return true;
}
static bool g_doorNativePriority=true, g_camButton=false;
enum { ON_DOORS, LN_DOOR };
static bool g_on[1]={true}, s_gripDown[2]={}, s_gripEdge[2]={}, s_nearKnob[2]={};
static V3 s_headW={20,20,0};
static float s_cy=1, s_sy=0;
static int s_bat=0;
static void ILog(const char*, ...) {}
static void Learn(int) {}
static long InterlockedExchange(volatile long* p,long v) { long old=*p; *p=v; return old; }
static void* OLUE3_Hero() { return &hero; }
static int OLUE3_DoorKnobWorld(void*,float out[3]) { out[0]=90; out[1]=out[2]=0; return 1; }
static int OLUE3_DoorHinge(void*,float out[3]) { out[0]=out[1]=out[2]=0; return 1; }
static int OLUE3_ObjectsOfClass(const char*,void** out,int) { out[0]=&door; return 1; }
static void OLUE3_DoorEventHook(void*) {}
static int OLUE3_DoorTriggerEvent(void*,int) { return 1; }
static void OLUE3_HeroMoveState(int* lm,int* sm) { *lm=*sm=0; }
static void OLXR_HapticPulse(int,float,int) {}
static int OLUE3_DoorSetRatioAndTarget(void* p,float r) {
    ++writes; auto* d=static_cast<FakeDoor*>(p); d->ratio=d->target=r; return 1;
}
'''

CASES = r'''
static bool ClimbHolding(int) { return false; }
static float BodyYaw() { return 0; }
static bool CamInRightHand() { return false; }
static int failures=0;
static void Check(bool ok,const char* name) {
    printf("%s: %s\n",ok?"PASS":"FAIL",name); if(!ok) ++failures;
}
static void Reset(unsigned char state=1,float target=0.6f) {
    door={}; door.ratio=0.6f; door.target=target; door.maxAngle=95; door.state=state;
    hero={}; hero.loc={-200,-200,0}; writes=0;
    for(auto& s:s_sim) s={};
    for(int h=0;h<2;++h) {
        s_grab[h]={}; s_gripDown[h]=s_gripEdge[h]=false;
        s_pushDoor[h]=nullptr; s_pushSide[h]=0;
    }
    s_all[0]=&door; s_allN=1; s_allMs=1000;
    s_sim[0].d=&door; s_sim[0].live=true; s_sim[0].lastCmd=0.6f; s_sim[0].om=-70;
    s_pushDoor[0]=&door; s_pushSide[0]=1;
    g_doorNativePriority=true; g_on[ON_DOORS]=true;
}
static void Tick(bool openHand=true) {
    V3 hands[2]={{45,3,0},{0,0,0}};
    bool valid[2]={openHand,false};
    Doors(1000,0.016f,hands,valid);
}
int main() {
    // Hero is idle, grip is released, and the hand overlaps the moving panel.
    Reset(2); Tick();
    Check(writes==0 && !s_sim[0].live && door.state==2,"native closing wins over open hand");
    Reset(2); Tick(false);
    Check(writes==0 && !s_sim[0].live,"native closing wins over residual swing");
    Reset(1,0); Tick();
    Check(writes==0 && door.target==0 && !s_sim[0].live,"new close target wins before the angle moves");
    Reset(1,0); door.ratio=0.599f; Tick();
    Check(writes==0 && door.target==0,"small native movement cannot be overwritten");
    Reset(1,1); Tick();
    Check(writes==0 && door.target==1,"new native opening target also wins");
    Reset(); Tick();
    Check(writes>0,"idle door still follows hand physics");
    Reset(1,0.61f); s_sim[0].lastCmd=0.61f; Tick();
    Check(writes>0,"own fallback target is not mistaken for a native command");
    Reset(2); g_doorNativePriority=false; Tick();
    Check(writes>0,"legacy setting restores closing behavior");
    Reset(1,0); g_doorNativePriority=false; Tick();
    Check(writes>0,"legacy setting restores target behavior");
    Reset(3); Tick(); Check(writes==0,"player interaction remains native");
    Reset(4); Tick(); Check(writes==0,"animation remains native");
    Reset(); door.broken=1; Tick(); Check(writes==0,"broken door remains native");
    Reset(); door.user=&failures; Tick(); Check(writes==0,"AI door remains native");
    Reset(); door.bits=F.mBusyBits; Tick(); Check(writes==0,"busy flags remain native");
    Reset(); door.bits=F.mLocked; Tick(); Check(writes==0,"locked door is not moved");
    Reset(); door.bits=F.mCant; Tick(); Check(writes==0,"cannot-close door is not closed");
    Reset(); door.type=1; Tick(); Check(writes==0,"lockers are excluded");
    Reset(); F.tgtR=0; Tick(); F.tgtR=offsetof(FakeDoor,target);
    Check(writes>0,"unreadable target does not invent native ownership");
    Reset(); door.target=std::numeric_limits<float>::quiet_NaN(); Tick();
    Check(writes>0,"invalid target does not invent native ownership");
    Reset(2); g_on[ON_DOORS]=false; Tick(); Check(writes==0,"DoorGrab=0 disables hand physics");
    return failures?1:0;
}
'''

with tempfile.TemporaryDirectory(prefix="outlast-door-test-") as tmp:
    cpp = Path(tmp) / "test.cpp"
    binary = Path(tmp) / "test"
    cpp.write_text(STUBS + door_code + CASES)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-variable", "-Wno-misleading-indentation",
                    str(cpp), "-o", str(binary)], check=True)
    raise SystemExit(subprocess.run([str(binary)]).returncode)
