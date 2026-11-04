"""The Beta 1.2.2 optimization pass: the work-avoidance rules, and that they default to ON.

Two things are proved here:

1. The one piece of new arithmetic - the object scan's time budget - behaves: a budget of 0 means
   "no bound" (the old fixed slice), and a slice is cut only when it has actually overspent.
2. Every switch added in the pass exists in the shipped ini with the OPTIMIZED default, is read by
   the code with that same default, and the hot paths really do go through the cached form instead
   of the per-call queries they replaced. That last part is what stops a future edit from quietly
   restoring a cross-module call inside the per-draw path.

Run: python3 tests/perf_switches_test.py
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
UE3 = (ROOT / 'src/ol_ue3.cpp').read_text()
PROXY = (ROOT / 'src/d3d9_proxy.cpp').read_text()
XR = (ROOT / 'src/ol_xr.cpp').read_text()
INI = (ROOT / 'runtime/outlastvr.ini').read_text()

# ---------------------------------------------------------------- 1. the time budget, compiled
logic = UE3[UE3.index('// [PERF-A7] The time budget'):
            UE3.index('static int ObjScanAll(void* c, void** out, int max)')]

checks = ('#include <cassert>\n#include <cstdio>\nusing LONG = long;\n' + logic + r'''
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // 0 = no bound: the slice is never cut early, whatever it cost (the pre-1.2.2 fixed slice).
    assert(!ObjScanOverBudget(0.0,   0));
    assert(!ObjScanOverBudget(999.0, 0));
    // A budget cuts only after it is exceeded - a slice that finishes inside it is not cut.
    assert(!ObjScanOverBudget(100.0, 250));
    assert(!ObjScanOverBudget(250.0, 250));   // exactly at budget: still within it
    assert( ObjScanOverBudget(250.1, 250));
    assert( ObjScanOverBudget(9.0,    5));
    std::puts("ok");
    return 0;
}
''')
with tempfile.TemporaryDirectory() as td:
    src = Path(td) / 't.cpp'
    exe = Path(td) / 't.exe'
    src.write_text(checks)
    subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-o', str(exe), str(src)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    assert out.strip() == 'ok', out
print('object scan budget: compiled checks passed')

# The budget can cut a slice short ANYWHERE, including inside the last stretch of the table. The
# pass may only be published once the cursor has actually reached the end, or a half-filled pass of
# doors/ledges/ladders would go live. (The first cut of this code tested the slice end and had
# exactly that bug.)
scan = UE3[UE3.index('extern "C" void OLUE3_ObjScanStep()'):
           UE3.index('// The head-centre world position and body yaw of the newest view')]
assert 'if (g_oscCursor < num) return;' in scan, \
    'the scan pass can publish a half-filled list when the budget cuts the last slice'
assert 'ObjScanOverBudget(PerfQpcUs() - tScan0, budgetUs)' in scan, 'the budget is no longer applied'
assert 'g_oscCursor = i;' in scan, 'the cursor is no longer kept across frames'

# ------------------------------------------------------------ 2. every switch exists, ON by default
# (key, section, default in the ini, default the code asks for)
SWITCHES = [
    ('DrawFastFlags',    'Perf', '1',   '1'),
    ('BackBufferCache',  'Perf', '1',   '1'),
    ('SkelFastGate',     'Perf', '1',   '1'),
    ('FovCache',         'Perf', '1',   '1'),
    ('NvLightCache',     'Perf', '1',   '1'),
    ('HudSmart',         'Perf', '1',   '1'),
    ('ObjScanBudgetUs',  'Perf', '250', '250'),
]
SOURCE = PROXY + UE3 + XR
for key, sec, ini_default, code_default in SWITCHES:
    assert f'{key}={ini_default}' in INI, f'{key} missing from the shipped ini (or not {ini_default})'
    pat = re.compile(r'L"%s",\s*L"%s",\s*%s' % (sec, key, code_default))
    assert pat.search(SOURCE), f'{key} is not read as [{sec}] {key}={code_default} in the code'
assert '[Perf]' in INI
print('ini/code defaults: all', len(SWITCHES), 'switches agree')

# ------------------------------------------------- 3. the hot paths really use the cached form
# A4: the per-draw predicate must not reach across translation units any more.
occl = PROXY[PROXY.index('static inline bool OcclDrawSkipped()'):
             PROXY.index('static HRESULT STDMETHODCALLTYPE Hook_DrawPrimitive(')]
assert 'FastFlags()' in occl, 'OcclDrawSkipped lost its flag snapshot'
assert 'OLUE3_OcclBypassOn' not in occl, 'OcclDrawSkipped is calling across modules again'
m0 = PROXY.index('static HRESULT MirrorUiDraw(')
mirror = PROXY[m0:m0 + 900]
head = mirror.split('kNativeEyeReplayEnabled')[0]
assert 'FastFlags()' in head, 'MirrorUiDraw lost its flag snapshot'
assert 'OLUE3_StereoMode' not in head and 'OLUE3_SplitActive' not in head, \
    'MirrorUiDraw calls OLUE3_StereoMode/SplitActive per draw again'

# A5: the cache is invalidated wherever the backbuffer can move under it.
for hook in ('static HRESULT STDMETHODCALLTYPE Hook_Reset',
             'static HRESULT STDMETHODCALLTYPE Hook_CreateDevice'):
    body = PROXY[PROXY.index(hook):PROXY.index(hook) + 700]
    assert 'InvalidateBackBufferCache();' in body, f'{hook} no longer invalidates the backbuffer cache'

# D1: the skeletal hook rejects other components BEFORE the solver's gate runs.
s0 = UE3.index('static void __fastcall Hook_SkelUpdate')
skel = UE3[s0:s0 + 1000]
assert 'PoseIsMilesComponent(comp)' in skel, 'Hook_SkelUpdate lost the component screen'
assert skel.index('PoseIsMilesComponent(comp)') < skel.index('PoseApplyVisible(comp)'), \
    'Hook_SkelUpdate no longer screens the component before the solver'
assert 'PoseRestoresIfBlocked()' in skel, 'the global restores are no longer kept for skipped components'
restores = UE3[UE3.index('static void PoseRestoresIfBlocked()'):
               UE3.index('static void PoseRestoresIfBlocked()') + 700]
for fn in ('CamRestore()', 'BatteryRestore()', 'DocumentRestore()'):
    assert fn in restores, f'{fn} dropped from the once-per-frame restore'
assert 's_doneFrame' in restores, 'the once-per-frame guard is gone'
print('A4/A5/D1: hot paths use the cached form')

# D3: the FOV cache must key on the frame AND on the engine's own value AND on the zoom - a hit with
# different inputs would hand the engine an angle it did not ask for.
f0 = UE3.index('static float __fastcall Hook_GetFovAngle')
fov = UE3[f0:f0 + 1000]
for part in ('OLProxy_PresentCount()', 'real == g_fovCacheReal', 'zoom == g_fovCacheZoom'):
    assert part in fov, f'FOV cache lost its key: {part}'
assert 'return FovModulateFrom(real);' in fov, 'the uncached path is gone from the FOV hook'
# ...and the shipped computation itself is untouched: it is still the only place that widens.
mod = UE3[UE3.index('static float FovModulateFrom(float real)'):
          UE3.index('} __except (EXCEPTION_EXECUTE_HANDLER) { return real; }')]
assert 'g_fovWrittenTan = tanf(w * 0.00872665f);' in mod, 'the widened-angle write moved or vanished'
assert 'g_nativeNarrowFrames, 4' in mod, 'the cinematic detector moved or vanished'

# D4: a remembered light is still a *candidate*, and the delta is still applied the same way.
d4 = UE3[UE3.index('static bool NvIsKnownPlayerComp'):UE3.index('static bool NvIsKnownPlayerComp') + 2600]
assert 'NvRememberPlayerComp(comp)' in d4 and 'mod[0] = rot[0] + dp' in d4, \
    'the NV light path changed shape'

# A6: the HUD upload is behind the change test and still fills an image that was never written.
h0 = XR.index('// [PERF-A6] THE READOUTS ARE REDRAWN')
hud = XR[h0:XR.index('// [PERF-A6] An image that already carries')]
assert 'stateChanged' in hud and 'OLCamHud::Top(' in hud
u0 = XR.index('// [PERF-A6] An image that already carries')
upload = XR[u0:u0 + 800]
assert 'if (stateChanged || !g_hudInitialized[index])' in upload, \
    'the upload is no longer guarded (a fresh swapchain image would stay blank)'

# The GPU timer never waits and never flushes: a blocking GetData would cost more than it measures.
gpu = XR[XR.index('static void GpuTimerSample()'):XR.index('static void GpuTimerStart()')]
assert 'D3D11_ASYNC_GETDATA_DONOTFLUSH' in gpu, 'the GPU timer is not asynchronous any more'
assert 'D3D11_GETDATA_FLUSH' not in gpu, 'the GPU timer flushes - it would stall the worker'

print('D3/D4/A6/GPU timer: all checks passed')


