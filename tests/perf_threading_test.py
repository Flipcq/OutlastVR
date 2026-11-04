"""Phase 3 of the Beta 1.2.2 pass: timer/scheduling, the pacing wait, and the monitor's owner.

Three things are proved here, all from the sources (the mod builds with MSVC and this suite runs
where there is no compiler):

1. A1 raises the process timer and boosts the XR worker before it can miss a compositor deadline,
   and the build links the two libraries those calls need.
2. A2 waits on g_conSeq with WaitOnAddress and is woken where the frame is really consumed; the
   old YieldProcessor spin survives ONLY behind [Perf] PaceWait=0.
3. A8 stands the render thread down only on the bridge's word (OLXR_MonitorActive), only while
   stereo is live, and the old mirror + windowed Present stay as the fallback.

Run: python3 tests/perf_threading_test.py
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
XR = (ROOT / 'src/ol_xr.cpp').read_text(encoding='utf-8')
PROXY = (ROOT / 'src/d3d9_proxy.cpp').read_text(encoding='utf-8')
INI = (ROOT / 'runtime/outlastvr.ini').read_text(encoding='utf-8')
BUILD = (ROOT / 'scripts/build.bat').read_text(encoding='utf-8')

# --------------------------------------------------------------- A1: timer, thread, EcoQoS
assert '#include <timeapi.h>' in XR, 'timeBeginPeriod is not declared'
raise_fn = XR[XR.index('static void PerfRaiseTimerResolution()'):
              XR.index('static void PerfBoostCurrentThread')]
assert 'timeBeginPeriod(1)' in raise_fn, 'the process timer is never raised'
assert 'g_timerResRaised' in raise_fn, 'the timer raise lost its once-only guard'
assert 'TIMERR_NOERROR' in raise_fn, 'the failure path of timeBeginPeriod is unchecked'

boost = XR[XR.index('static void PerfBoostCurrentThread'):
           XR.index('static void PerfRaiseProcessPriority')]
assert 'THREAD_PRIORITY_TIME_CRITICAL' in boost, 'the worker is not time-critical'
assert 'ThreadPowerThrottling' in boost and 'THREAD_POWER_THROTTLING_EXECUTION_SPEED' in boost, \
    'the EcoQoS opt-out is gone'
assert 'StateMask = 0' in boost, 'the EcoQoS opt-out no longer says "never throttle"'

init = XR[XR.index('static bool Init('):XR.index('XLOG("[OLVR][XR] session created")')]
assert 'PerfRaiseTimerResolution();' in init, 'Init does not raise the timer'
assert 'PerfRaiseProcessPriority();' in init, 'Init does not apply the optional process boost'

worker = XR[XR.index('static DWORD WINAPI WorkerMain(LPVOID) {'):]
assert worker.index('PerfBoostCurrentThread(') < worker.index('while (!g_workerStop)'), \
    'the worker is boosted after its loop starts (or not at all)'

assert 'winmm.lib' in BUILD, 'timeBeginPeriod is not linked (winmm.lib)'

# --------------------------------------------------------------- A2: WaitOnAddress instead of spin
pace = XR[XR.index('static void PaceToHeadset()'):XR.index('void OLXR_OnPresent(')]
assert 'WaitOnAddress((volatile void*)&g_conSeq, (void*)&done, sizeof(done), waitMs)' in pace, \
    'the pacing wait no longer blocks on g_conSeq'
assert 'if (!g_paceWait)' in pace, 'the A/B path for the old wait is gone'
guard = pace.index('if (!g_paceWait)')
assert 'YieldProcessor()' in pace and pace.index('YieldProcessor()') > guard, \
    'the spin escaped the [Perf] PaceWait=0 branch'
assert pace.index('Sleep(1)') > guard, 'the old Sleep(1) escaped its A/B branch'
assert 'want == 0 || done >= want' in pace, 'the "already consumed" test changed shape'

consume = XR[XR.index('static void ConsumeUpload()'):XR.index('extern "C" int  OLUE3_StereoMode();')]
assert 'bool advanced = false;' in consume, 'the consumer no longer tracks whether it advanced'
assert 'if (advanced) WakeByAddressAll((void*)&g_conSeq);' in consume, \
    'the consumer does not wake the pacing thread'
assert consume.index('WakeByAddressAll') > consume.index('ReleaseSRWLockShared(&g_pubLock);'), \
    'the pacer is woken while the lock is still held'
assert 'static volatile uint64_t g_conSeq' in XR, 'g_conSeq must be the exact 64-bit word waited on'
assert 'synchronization.lib' in BUILD, 'WaitOnAddress is not linked (synchronization.lib)'

# --------------------------------------------------------------- A8: the monitor moves to the bridge
mon = XR[XR.index('static void MonitorPresent()'):XR.index('extern "C" int OLXR_MonitorActive()')]
assert 'if (!g_bridgeMirrorOn || !g_useWorker || !g_running)' in mon, \
    'the [Perf] BridgeMirror switch or the worker guard no longer disables the path'
assert 'DXGI_PRESENT_DO_NOT_WAIT' in mon, 'the monitor Present can block the XR worker'
sc = XR[XR.index('static bool MonitorEnsureSwapchain'):XR.index('static bool MonitorDraw')]
assert 'DXGI_SWAP_EFFECT_DISCARD' in sc, 'the monitor swap chain is not the bitblt model'
assert 'CreateSwapChain(g_d3d11' in sc, 'the monitor swap chain is not created on the bridge device'
assert 'QueryInterface(__uuidof(IDXGIDevice)' in sc and 'GetAdapter(&ad)' in sc, \
    'the monitor swap chain factory is not the D3D11 device adapter\'s'
assert 'g_monRetryTick' in sc and 'now < retry' in sc, \
    'a refused swap chain would be re-created on every monitor frame'
assert 'OLProxy_MirrorFps()' in mon, 'the monitor ignores [Render] DesktopMirrorFps'
assert 'OLProxy_MirrorOneEye()' in mon, 'the monitor ignores [Render] DesktopMirrorOneEye'
draw = XR[XR.index('static bool MonitorDraw'):XR.index('// Called from the worker:')]
assert 'boxW = oneEye ? g_w / 2 : g_w' in draw, \
    'DesktopMirrorOneEye=0 no longer shows the raw side-by-side frame on the monitor'
assert 'IsIconic(hwnd)' in mon and 'IsWindowVisible(hwnd)' in mon, \
    'the monitor presents into a hidden/minimised window'
assert '!g_conSeq' in mon, 'the monitor can present before a frame was ever consumed'
watchdog = XR[XR.index('extern "C" int OLXR_MonitorActive()'):]
assert 'LONG64 tol = 400;' in watchdog and 'GetTickCount64() - last) < tol' in watchdog, \
    'the fallback watchdog is gone or no longer time-based'
assert '2000 / (LONG64)fps + 100' in watchdog, \
    'a very low DesktopMirrorFps would expire the watchdog between two presents'

worker = XR[XR.index('static DWORD WINAPI WorkerMain(LPVOID) {'):]
assert worker.index('ConsumeUpload();') < worker.index('MonitorPresent();'), \
    'the monitor presents before the frame has been uploaded'
assert worker.index('MonitorPresent();') < worker.index('RunXrFrame('), \
    'the monitor waits behind xrWaitFrame'

present = PROXY[PROXY.index('static HRESULT STDMETHODCALLTYPE Hook_Present'):
                PROXY.index('// [SCREQ] the WINDOW lever')]
assert 'if (split && g_xrAllowed && g_xrEnabled && OLXR_MonitorActive())' in present, \
    'the render thread stands down on anything but the bridge\'s word + a live split'
a8 = present.index('OLXR_MonitorActive()')
assert a8 < present.index('DrawOneEyeDesktopMirror();'), 'the old mirror still runs before A8'
assert a8 < present.index('o_Present(This, a, b, c, d)'), 'the windowed Present still runs before A8'
assert 'DrawOneEyeDesktopMirror();' in present, 'the fallback mirror was deleted'
for decl in ('extern "C" HWND OLProxy_GameWindow()',
             'extern "C" int  OLProxy_IsWindowed()',
             'extern "C" long OLProxy_MirrorFps()',
             'extern "C" int  OLProxy_MirrorOneEye()'):
    assert decl in PROXY, f'{decl} is missing from the proxy'

# --------------------------------------------------------------- the switches and their defaults
SWITCHES = [
    ('TimerResolution', '1', '1'),
    ('ThreadPriority',  '1', '1'),
    ('ProcessPriority', '0', '0'),
    ('PaceWait',        '1', '1'),
    ('BridgeMirror',    '1', '1'),
]
for key, ini_default, code_default in SWITCHES:
    assert f'{key}={ini_default}' in INI, f'{key} missing from the shipped ini (or not {ini_default})'
    pat = re.compile(r'L"Perf",\s*L"%s",\s*%s' % (key, code_default))
    assert pat.search(XR), f'{key} is not read as [Perf] {key}={code_default} in the code'

print('A1 timer/priority/EcoQoS: ok')
print('A2 WaitOnAddress pacing: ok')
print('A8 bridge-owned monitor with live fallback: ok')
print('ini/code defaults: all', len(SWITCHES), 'phase-3 switches agree')
