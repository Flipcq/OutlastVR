"""Phase 5 of the Beta 1.2.2 pass: the two eyes stop fighting over one visibility verdict.

Four things are proved here:

1. B1, the per-eye occlusion answer (`[OCCLPEREYE]`). The real table and the real decision code from
   d3d9_proxy.cpp are compiled against a stub device layer, so what is exercised is what ships: each
   query keeps one answer per eye, a query the GPU has not finished answers with THAT eye's previous
   frame instead of stalling, a query's very first read still asks the device the way the engine did,
   and the table is bounded - a pool bigger than the table asks the device rather than handing back
   somebody else's verdict.

2. B2 + the eye tracker (`[PVSSHARE]`, `[OCCLPEREYE]`). The real block from ol_ue3.cpp is compiled:
   the eye is identified by comparing the renderer's view matrices against the committed LEFT view,
   and the precomputed-visibility answer is shared by key, never across a frame boundary.

3. C1's mechanism (`[VISREUSE]`): the second eye's list is only offered when the previous frame's two
   passes asked the same number of questions - the self-check that makes call order a usable pairing.

4. C2 and the wiring: the union frustum is one frustum for both halves, the blit crops each half
   back with the frustum it was RENDERED with, and every switch exists in the shipped ini with the
   default the code asks for.

Run: python3 tests/perf_visibility_test.py
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROXY = (ROOT / 'src/d3d9_proxy.cpp').read_text(encoding='utf-8')
UE3 = (ROOT / 'src/ol_ue3.cpp').read_text(encoding='utf-8')
XR = (ROOT / 'src/ol_xr.cpp').read_text(encoding='utf-8')
INI = (ROOT / 'runtime/outlastvr.ini').read_text(encoding='utf-8')


def block(signature, src=None):
    """The whole function, from its signature to its matching closing brace."""
    text = src if src is not None else PROXY
    start = text.index(signature)
    i = text.index('{', start)
    depth = 0
    while i < len(text):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    raise AssertionError('unbalanced function: ' + signature)


# ------------------------------------------------------------ 1. the switches, ini and code agree
# (key, default in the ini, default the code asks for, source file)
SWITCHES = [
    ('OcclPerEye',   '1',  '1',  PROXY),
    ('PvsShare',     '1',  '1',  UE3),
    ('VisReuse',     '0',  '0',  PROXY),
    ('UnionFrustum', '0',  '0',  XR),
    ('UnionNearUU',  '10', '10', XR),
]
for key, ini_default, code_default, src in SWITCHES:
    assert f'{key}={ini_default}' in INI, f'{key}={ini_default} missing from the shipped ini'
    pat = re.compile(r'L"Perf",\s*L"%s",\s*%s' % (key, code_default))
    assert pat.search(src), f'{key} is not read as [Perf] {key}={code_default} in the passed source'
print('ini/code defaults: OcclPerEye=1, PvsShare=1, VisReuse=0, UnionFrustum=0, UnionNearUU=10')

# ------------------------------------------------------------------------------ 2. the stubs
stub = r'''
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cmath>
#include <cstddef>

typedef long HRESULT;
typedef unsigned long DWORD;
typedef unsigned int UINT;
using LONG = long;
#define S_OK 0L
#define S_FALSE 1L
#define STDMETHODCALLTYPE
struct IDirect3DQuery9 { };
struct IDirect3DDevice9 { };
struct D3DVIEWPORT9 { UINT X, Y, Width, Height, MinZ, MaxZ; };
typedef HRESULT (STDMETHODCALLTYPE *SetViewport_t)(IDirect3DDevice9*, const D3DVIEWPORT9*);

// what the code under test reads from the rest of the mod
static volatile LONG g_occlPerEye = 1;      // [Perf] OcclPerEye
static volatile LONG g_visReuse = 0;        // [Perf] VisReuse
static volatile LONG g_sawPrimaryEye = 0;   // [VISREUSE] guard: the primary view was seen this frame
static volatile long g_olRenderEye = 0;     // ol_ue3.cpp: which view the renderer is drawing
static UINT g_bbW = 2000;                   // the frame's width, as the proxy sees it
static double g_fakeNowMs = 0.0;
static HRESULT STDMETHODCALLTYPE StubSetViewport(IDirect3DDevice9*, const D3DVIEWPORT9*) { return S_OK; }
extern "C" double OLXR_PerfNowMs() { return g_fakeNowMs; }
#define InterlockedExchange(p, v) (*(p) = (v))
#define InterlockedIncrement(p)   (++*(p))
'''


# The per-eye block the d3d9 proxy ships, from the viewport mirror to the C1 bookkeeping. Everything
# it reads from the rest of the mod is stubbed above, so this is the real code that runs per query.
PER_EYE = PROXY[PROXY.index('static volatile LONG g_vpX = 0, g_vpW = 0;'):
               PROXY.index('// [OCCLSKIP] (Test46) With OcclForce')]
READ_PLAN = block('static int OcclReadPlan(int pollOk, int haveThisEye)')

per_eye_checks = r'''
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // ---- B1: the decision table -------------------------------------------------------------
    // A fresh answer is used as it is; a query the GPU has not finished answers with this eye's
    // previous frame; a query that has no previous frame yet asks the device exactly once.
    assert(OcclReadPlan(1, 0) == 0);
    assert(OcclReadPlan(1, 1) == 0);
    assert(OcclReadPlan(0, 1) == 1);
    assert(OcclReadPlan(0, 0) == 2);

    // An answer that stops arriving must not freeze a verdict: after kOcclStaleLimit consecutive
    // stale reads the caller has to ask the device the way the engine did, and the counter restarts.
    LONG run = 0;
    for (int i = 0; i < kOcclStaleLimit; ++i) assert(OcclStaleOk(&run) == 1);
    assert(OcclStaleOk(&run) == 0);
    run = 0;
    assert(OcclStaleOk(&run) == 1);
    assert(OcclStaleOk(nullptr) == 0);

    // ---- B1: one entry per query, and one answer per eye -------------------------------------
    IDirect3DQuery9* q1 = (IDirect3DQuery9*)(uintptr_t)0x100000;
    IDirect3DQuery9* q2 = (IDirect3DQuery9*)(uintptr_t)0x100010;
    OcclSlot* s1 = OcclSlotFor(q1, true);
    assert(s1 != nullptr && OcclSlotFor(q1, true) == s1);
    assert(OcclSlotFor(q2, true) != nullptr);
    assert(OcclSlotFor(nullptr, true) == nullptr);
    // The two eyes' answers live in different cells and cannot overwrite each other.
    s1->ans[0] = 4096; s1->have[0] = 1;
    assert(s1->have[1] == 0);                       // the other eye has nothing yet
    s1->ans[1] = 12;   s1->have[1] = 1;
    assert(s1->ans[0] == 4096 && s1->ans[1] == 12);

    // A table that cannot grow answers "ask the device", never somebody else's verdict.
    OcclTableReset();
    IDirect3DQuery9* many[kOcclSlots + 1];
    for (size_t i = 0; i < (size_t)kOcclSlots; ++i) {
        many[i] = (IDirect3DQuery9*)(uintptr_t)(0x200000 + i * 16);
        assert(OcclSlotFor(many[i], true) != nullptr);
    }
    many[kOcclSlots] = (IDirect3DQuery9*)(uintptr_t)0x300000;
    assert(OcclSlotFor(many[kOcclSlots], true) == nullptr);

    // A Reset/new device drops every answer.
    OcclTableReset();
    assert(OcclSlotFor(q1, false) == nullptr);
    assert(g_occlEyePass == -1);

    // ---- B1: which eye is asking ------------------------------------------------------------
    // A per-view viewport is the engine's own statement; a full-frame one leaves it to the
    // matrix compare the ol_ue3 side publishes.
    g_bbW = 2000;
    g_vpX = 0;    g_vpW = 1000;  assert(OcclEyeFromViewport() == 0);
    g_vpX = 1000; g_vpW = 1000;  assert(OcclEyeFromViewport() == 1);
    g_vpX = 0;    g_vpW = 2000;                 // full frame: no opinion
    g_olRenderEye = 1;           assert(OcclEyeFromViewport() == 1);
    g_olRenderEye = 0;           assert(OcclEyeFromViewport() == 0);

    // ---- C1's guard: only a frame that really saw the primary view may skip the 2nd eye's boxes
    g_vpW = 1000;                // a per-view viewport is in force again
    g_sawPrimaryEye = 0;
    g_vpX = 0;                   assert(OcclEyeNow() == 0 && g_sawPrimaryEye == 1);
    g_sawPrimaryEye = 0;         // what Present does at a frame boundary
    g_vpX = 1000;                assert(OcclEyeNow() == 1 && g_sawPrimaryEye == 0);

    // ---- C1: the second eye's list, and the self-check that arms it -------------------------
    OcclTableReset();                            // a clean pass state for the sequence below
    g_visReuse = 1;
    g_bbW = 2000; g_vpX = 0; g_vpW = 1000;      // left half
    g_fakeNowMs = 10.0;
    assert(OcclEyeNow() == 0);                   // opens the primary pass
    VisReuseRecord(0, 10); VisReuseRecord(0, 20); VisReuseRecord(0, 30);
    assert(g_visLN == 3);
    g_vpX = 1000;                                // right half: the second eye's pass
    g_fakeNowMs = 12.5;
    assert(OcclEyeNow() == 1);
    assert(g_visLNPrev == 3);                    // what the primary pass recorded
    DWORD v = 0;
    // FIRST frame: the two passes have not been seen to agree yet, so nothing is reused.
    assert(VisReuseTake(&v) == 0);
    VisReuseRecord(1, 7); VisReuseRecord(1, 7); VisReuseRecord(1, 7);
    assert(g_visIdx == 3);
    // The primary pass of the next frame: now the counts can be compared (3 vs 3).
    g_vpX = 0; g_fakeNowMs = 20.0;
    assert(OcclEyeNow() == 0);
    assert(g_visPairOk == 1);
    // the pass that just closed was timed, and the primary's list was cleared for the new frame
    assert(g_occlPassN[1] == 1 && g_occlPassUs[1] > 0.0);
    assert(g_visLN == 0);
    VisReuseRecord(0, 100); VisReuseRecord(0, 200); VisReuseRecord(0, 300);
    g_vpX = 1000; g_fakeNowMs = 22.0;
    assert(OcclEyeNow() == 1);
    // The second eye now reads the first eye's verdicts, in call order, and nothing else.
    assert(VisReuseTake(&v) && v == 100);
    assert(VisReuseTake(&v) && v == 200);
    assert(VisReuseTake(&v) && v == 300);
    assert(VisReuseTake(&v) == 0);               // the list is exhausted: ask the device
    VisReuseRecord(1, 7); VisReuseRecord(1, 7); VisReuseRecord(1, 7);

    // A frame whose two passes asked a different number of questions disarms the pairing.
    g_vpX = 0;  assert(OcclEyeNow() == 0);
    VisReuseRecord(0, 1); VisReuseRecord(0, 2);                          // 2 answers
    g_vpX = 1000; assert(OcclEyeNow() == 1);
    VisReuseRecord(1, 9); VisReuseRecord(1, 9); VisReuseRecord(1, 9);     // 3 reads
    g_vpX = 0;  assert(OcclEyeNow() == 0);
    assert(g_visPairOk == 0);
    g_vpX = 1000; assert(OcclEyeNow() == 1);
    assert(VisReuseTake(&v) == 0);

    std::puts("ok");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as td:
    src = Path(td) / 't.cpp'
    exe = Path(td) / 't.exe'
    src.write_text(stub + '\n' + PER_EYE + '\n' + READ_PLAN + '\n' + per_eye_checks)
    subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wno-unused-function',
                    '-o', str(exe), str(src)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    assert out.strip() == 'ok', out
print('B1/C1: per-eye answers, no stalls, bounded table, and a paired second pass')

# ------------------------------------------- 3. B2 + the eye tracker, compiled from ol_ue3.cpp
# The block that identifies the eye from the renderer's own matrices and shares the precomputed
# visibility answer between the two views. Its only outside dependency is the guarded memory read.
ue3_stub = r'''
#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstddef>
#include <cstdint>
using LONG = long;
static const size_t kViewMatOff = 0x80;      // FSceneView::ViewMatrix (confirmed live)
static bool SafeReadBytes(const void* src, void* dst, size_t n) { memcpy(dst, src, n); return true; }
#define InterlockedExchange(p, v) (*(p) = (v))
#define InterlockedIncrement(p)   (++*(p))
'''

EYE_AND_PVS = UE3[UE3.index('// ---- [OCCLPEREYE] WHICH EYE THE RENDERER IS DRAWING'):
                  UE3.index('// ---- the Outlast-custom occlusion tracker')]

ue3_checks = r'''
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // ---------------------------------------------------------- the eye the renderer is drawing
    float keyL[32], keyR[32], keyL2[32];
    for (int i = 0; i < 32; ++i) { keyL[i] = 1.0f + (float)i; keyR[i] = 10.0f + (float)i; keyL2[i] = 100.0f + (float)i; }
    unsigned char vL[0x100] = {0}, vR[0x100] = {0}, vL2[0x100] = {0};
    memcpy(vL + kViewMatOff,  keyL,  128);
    memcpy(vR + kViewMatOff,  keyR,  128);
    memcpy(vL2 + kViewMatOff, keyL2, 128);

    // Before any view has been committed there is nothing to compare against: the tracker answers
    // "the other eye", which keeps every per-eye path on ONE set of answers (the shared shape it
    // replaced) instead of splitting them the wrong way round.
    int np = 0;
    assert(EyeOfView(vR, &np) == 1 && np == 0);

    EyeNoteLeftView(keyL);
    np = 0;
    assert(EyeOfView(vL, &np) == 0 && np == 1);     // the frame's first view opens the primary pass
    assert(g_olRenderFrame == 1);
    // The engine walks many primitives per view and they all carry the same matrices: that is NOT a
    // new pass, and the per-frame caches must not be thrown away for every primitive.
    assert(EyeOfView(vL, &np) == 0 && np == 0);
    assert(g_olRenderFrame == 1);
    assert(EyeOfView(vR, &np) == 1 && np == 0);     // the other eye, and only it
    assert(g_olRenderEye == 1);
    // A new frame is a new LEFT key.
    EyeNoteLeftView(keyL2);
    assert(EyeOfView(vL2, &np) == 0 && np == 1);
    assert(g_olRenderFrame == 2);
    assert(g_perfEyePrimary >= 3 && g_perfEyeSecondary >= 1);

    // ------------------------------------------------------------ the shared precomputed visibility
    long long out = 0;
    int states[8] = {0};
    PvsShareNewPass();
    assert(PvsShareLookup(&states[0], &out) == 0);
    PvsShareStore(&states[0], 0x1111);
    PvsShareStore(&states[1], 0x2222);
    // keyed by the call's own state pointer, so the order the renderer walks primitives in does not
    // matter: both directions hit, and an unknown key misses.
    assert(PvsShareLookup(&states[1], &out) == 1 && out == 0x2222);
    assert(PvsShareLookup(&states[0], &out) == 1 && out == 0x1111);
    assert(PvsShareLookup(&states[2], &out) == 0);
    assert(PvsShareLookup(nullptr, &out) == 0);
    PvsShareStore(nullptr, 0x9999);                 // never crash on a null key
    // Re-storing the same key replaces the answer.
    PvsShareStore(&states[0], 0x3333);
    assert(PvsShareLookup(&states[0], &out) == 1 && out == 0x3333);

    // A pointer from a frame that is gone must never be handed out: both the explicit pass reset and
    // the per-Present boundary drop everything.
    PvsShareNewPass();
    assert(PvsShareLookup(&states[0], &out) == 0);
    PvsShareStore(&states[3], 0x4444);
    assert(PvsShareLookup(&states[3], &out) == 1);
    OLUE3_PerfFrameBoundary();
    assert(PvsShareLookup(&states[3], &out) == 0);

    // The table is bounded: more live keys than slots do not grow it and do not crash.
    PvsShareNewPass();
    for (long i = 0; i < (long)kPvsShareSlots + 200; ++i)
        PvsShareStore((const void*)(uintptr_t)(0x10000 + i * 64), (long long)i);
    assert(g_pvsShareFull > 0);
    PvsShareNewPass();                              // and the next pass starts clean again

    // The counters are taken and cleared, so a [PERF] line never reports the same work twice.
    g_pvsShareServed = 3; g_pvsShareMiss = 2;      // what Hook_PvsGet would have counted
    long served = 0, miss = 0, full = 0, pri = 0, sec = 0;
    OLUE3_TakeVisibilityStats(&served, &miss, &full, &pri, &sec);
    assert(served == 3 && miss == 2 && full > 0 && pri > 0 && sec > 0);
    OLUE3_TakeVisibilityStats(&served, &miss, &full, &pri, &sec);
    assert(served == 0 && miss == 0 && full == 0 && pri == 0 && sec == 0);

    std::puts("ok");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as td:
    src = Path(td) / 't.cpp'
    exe = Path(td) / 't.exe'
    src.write_text(ue3_stub + '\n' + EYE_AND_PVS + '\n' + ue3_checks)
    subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wno-unused-function',
                    '-o', str(exe), str(src)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    assert out.strip() == 'ok', out
print("B2/eye: the eye comes from the renderer's own matrices, and PVS is shared inside one frame")



# ------------------------------------------------- 4. C2 and the wiring of every phase-5 piece
# The union frustum must be ONE frustum: the same numbers for both halves, and the blit must crop
# each half back with the frustum that half was RENDERED with (its own lens in lens mode, the union
# in union mode).
lens = block('extern "C" int OLXR_LensFitTangents(int half, float* l, float* r, float* u, float* d) {', XR)
assert 'if (g_unionFrustumOn) return OlrUnionTangents(l, r, u, d);' in lens, \
    'the union frustum is no longer what both halves are built with'
union = block('static int OlrUnionTangents(float* l, float* r, float* u, float* d)', XR)
assert 'int half' not in union, 'the union frustum depends on which half asks'
assert 'fminf(al, bl) - off' in union and 'fmaxf(ar, br) + off' in union, \
    'the horizontal union no longer takes the wider edge of both eyes plus the stereo offset'
assert 'fmaxf(au, bu)' in union and 'fminf(ad, bd)' in union, 'the vertical union changed shape'
blit_site = XR[XR.index('if (fitMode) {'):XR.index('if (!fittedToRuntime)')]
assert 'OLXR_LensFitTangents(0,' in blit_site and 'OLXR_LensFitTangents(1,' in blit_site, \
    'the blit no longer asks which frustum each half was rendered with'
print('C2: one frustum for both halves, cropped back per eye by the blit')

# B1: the per-eye path owns the queries while it is on, and the force-visible path is untouched.
body = block('static HRESULT STDMETHODCALLTYPE Hook_QueryGetData(IDirect3DQuery9* This, void* pData, DWORD size, DWORD flags)')
assert 'if (occl && g_occlPerEye && pData && size >= sizeof(DWORD))' in body, \
    'the per-eye path is gone (or no longer gated on OcclPerEye)'
assert 'flags & ~(DWORD)D3DGETDATA_FLUSH' in body, 'a read can flush and stall again'
assert body.index('flags & ~(DWORD)D3DGETDATA_FLUSH') < body.index('OLUE3_OcclBypassOn()'), \
    'the per-eye path no longer comes first'
assert '0x7FFFFF' in body.split('OLUE3_OcclBypassOn()')[1], 'the force-visible fallback was removed'
# The boxes ARE the answer while the per-eye path runs: only C1's second eye may skip them.
skip = block('static inline bool OcclDrawSkipped()')
assert 'if (g_occlPerEye) {' in skip, 'the box test no longer knows about the per-eye path'
assert 'if (!g_visReuse || !(FastFlags() & kFF_VisReuse) || !g_sawPrimaryEye) return false;' in skip, \
    'occlusion boxes can be skipped while their answer is the one being used (or without the guard)'
assert 'if (g_olRenderEye == 0) return false;' in skip, 'the FIRST eye lost its boxes'
assert 'if (e == 0) InterlockedExchange(&g_sawPrimaryEye, 1);' in PER_EYE, \
    'the C1 guard is no longer armed by the primary pass'
present = block('static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* This, const RECT* a, const RECT* b, HWND c, const RGNDATA* d)')
assert 'OLUE3_PerfFrameBoundary();' in present, \
    'Present no longer closes the frame the PVS cache is scoped to'
assert 'kFF_VisReuse = 8' in PROXY and 'if (OLXR_UnionFrustumOn()) f |= kFF_VisReuse;' in PROXY, \
    'C1 is no longer gated on C2 in the per-Present snapshot'
assert 'HookSlot(vt, 47, (void*)&Hook_SetViewport)' in PROXY, 'the viewport mirror is not hooked'
for where in ('static HRESULT STDMETHODCALLTYPE Hook_Reset', 'static void InstallDeviceHooks'):
    assert 'OcclTableReset();' in PROXY[PROXY.index(where):PROXY.index(where) + 4000], \
        f'{where} keeps queries (and their answers) across a device change'
# The eye key is committed with the frame, from the same matrices [FRAMETAG] uses.
assert 'EyeNoteLeftView(key);' in UE3, 'the LEFT view key is no longer recorded for the eye tracker'
pvs = block('static long long __fastcall Hook_PvsGet(void* state, void* view, void* scene)', UE3)
assert 'EyeOfView(view, nullptr)' in pvs, 'the PVS hook lost the eye'
assert pvs.index('if (eye == 0)') < pvs.index('PvsShareLookup'), \
    'the second eye can store into the shared answer'
print('B1/B2/C1: the wiring - per-eye first, boxes kept, C1 gated on C2, caches dropped at the edges')

print('\nall phase-5 checks passed')
