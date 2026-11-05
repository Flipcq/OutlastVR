"""Phase 4 of the Beta 1.2.2 pass: the frame's picture is read where it is, and the views are kept.

Three things are proved here:

1. A3, the copy that vanished. With [EXSHARE] the bridge used to copy every frame (36 MB at
   3456x1824) out of the shared texture before blitting. The blits now sample the shared texture
   itself, and the source of the picture is a single pointer (`FrameTex()`) that every consumer goes
   through - the src scan below is what keeps it that way.

2. A3, the view cache. `ViewCacheGet` is compiled here from the REAL source against a stub D3D11
   layer, so what is checked is the code that ships: one view per (texture, kind, format), a view
   that is reused on the next frame instead of being made again, an entry that holds its texture (so
   a rebuilt swapchain can never leave a stale pointer behind), and a table that drops everything
   when it runs out of slots or is cleared at a rebuild.

3. B8, the direct draw. `OlrDirectViewFormat` is compiled too: it must take the view format AND the
   colour space from the destination image, refuse anything it does not know, and refuse a
   destination that is not a render target at all - that refusal is the automatic fallback.

Run: python3 tests/perf_framepath_test.py
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
XR = (ROOT / 'src/ol_xr.cpp').read_text(encoding='utf-8')
INI = (ROOT / 'runtime/outlastvr.ini').read_text(encoding='utf-8')


def block(signature):
    """The whole function, from its signature to its matching closing brace."""
    start = XR.index(signature)
    i = XR.index('{', start)
    depth = 0
    while i < len(XR):
        if XR[i] == '{':
            depth += 1
        elif XR[i] == '}':
            depth -= 1
            if depth == 0:
                return XR[start:i + 1]
        i += 1
    raise AssertionError('unbalanced function: ' + signature)


# ---------------------------------------------------------------- the switches, ini and code agree
# SharedFrameDirect ships ON (with its own automatic fallback), DirectSwapchain stays OFF, and
# ViewCache ships on because it is the transparent part of A3.
SWITCHES = [('SharedFrameDirect', '1'), ('DirectSwapchain', '0'), ('ViewCache', '1')]
for key, default in SWITCHES:
    assert f'{key}={default}' in INI, f'{key}={default} missing from the shipped ini'
    pat = re.compile(r'L"Perf",\s*L"%s",\s*%s' % (key, default))
    assert pat.search(XR), f'{key} is not read as [Perf] {key}={default} in the code'
print('ini/code defaults: SharedFrameDirect=1, DirectSwapchain=0, ViewCache=1')

# With SharedFrameDirect off, "the frame" must be exactly the upload texture: the phase-4 indirection
# has to be invisible, or the 0 setting would not restore the 1.2.2 path.
assert 'static volatile LONG   g_sharedFrameDirect = 1;' in XR, \
    'SharedFrameDirect no longer ships on - the validated hardware path is not the default'
assert 'return g_frameTex ? g_frameTex : g_uploadTex;' in XR, 'FrameTex lost its fallback to the upload texture'

# --------------------------------------------------- 1. A3: the picture is read where it lies
# Every consumer of "this frame's pixels" goes through FrameTex(); nothing blits the upload texture
# directly any more (that would silently skip the shared frame on the direct lane).
for gone in ('GammaDraw(g_uploadTex', 'CreateShaderResourceView(g_uploadTex',
             'CreateShaderResourceView(src, nullptr', 'CopyResource(g_xrImages[idx], g_uploadTex)',
             'CopyResource(g_eyeHold[cur], g_uploadTex)'):
    assert gone not in XR, f'{gone} is still reading the upload texture behind the frame source'
for user in ('static bool MonitorDraw(', 'static bool FlipScreenLayer(', 'static bool BlitHalfToEye(',
             'static bool BlitFitToEyeTan(', 'static bool BlitToSwapchain(', 'static void DevSaveUploadTex('):
    start = XR.index(user)
    assert 'FrameTex()' in XR[start:start + 8000], f'{user} no longer reads the frame through FrameTex()'
# The two helpers that take the picture as an argument are fed from the same single source.
assert 'CaptureHaloPalette(FrameTex()' in XR, 'the halo still of the menu bypasses the frame source'
assert 'CachedSrv(src)' in block('static bool CaptureHaloPalette('), 'the halo capture makes a view per call'
assert 'BlitHoldToEye(g_scL, g_imgL, g_eyeHold[0])' in XR, 'the AER lane changed shape'
assert 'g_ctx->CopyResource(g_eyeHold[cur], FrameTex());' in XR, 'the AER hold copies the wrong texture'
# The direct lane only ever points at a texture that the slot keeps alive until the draws are queued.
take = block('static bool ExWorkerTake(long idx)')
assert 'g_frameTex = s.tex11;' in take, 'the direct lane does not adopt the shared texture'
assert 'ExDirectRelease();' in take, 'the previous read window is never closed'
rel = block('static void ExDirectRelease() {')   # the definition, not the forward declaration
assert 'g_ctx->End(s.done11)' in rel, 'the slot is handed back without waiting for the draws'
assert 's.state == 3' in rel, 'the release does not check that the slot is still ours'
tick = block('static void ExWorkerTick()')
assert '== g_exDirectSlot' in tick and 'continue;' in tick, \
    "ExWorkerTick can free the slot being read (its query is still the previous frame's)"
assert 'CopyResource(g_uploadTex, s.tex11)' in take, 'the automatic fallback to the copy is gone'
print('A3: one frame source, held until the draws are queued, with the copy as the fallback')

# ------------------------------------------------------ 2/3. the real code, compiled and exercised
stub = r'''
#include <cstdio>
#include <cassert>
#include <cstdarg>
'''

# the source calls the shared logger; the harness only needs it to exist
stub += r'''
static void XLOG(const char* fmt, ...) { (void)fmt; }
'''
stub += r'''
typedef long HRESULT;
#define S_OK 0L
#define E_FAIL ((HRESULT)0x80004005L)
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr)    ((HRESULT)(hr) < 0)
typedef int DXGI_FORMAT;
#define DXGI_FORMAT_UNKNOWN              0
#define DXGI_FORMAT_R8G8B8A8_UNORM       28
#define DXGI_FORMAT_B8G8R8A8_UNORM       87
#define DXGI_FORMAT_B8G8R8A8_TYPELESS    90
#define DXGI_FORMAT_B8G8R8A8_UNORM_SRGB  91
enum { D3D11_BIND_SHADER_RESOURCE = 0x8, D3D11_BIND_RENDER_TARGET = 0x20 };
enum { D3D11_RTV_DIMENSION_TEXTURE2D = 4 };
struct D3D11_TEXTURE2D_DESC { DXGI_FORMAT Format; unsigned BindFlags; };
struct D3D11_RENDER_TARGET_VIEW_DESC { DXGI_FORMAT Format; int ViewDimension; };
static int g_srvCreated = 0, g_rtvCreated = 0, g_viewReleased = 0, g_texReleased = 0;
struct ID3D11View { long refs = 1; void Release() { --refs; ++g_viewReleased; } };
struct ID3D11ShaderResourceView : ID3D11View {};
struct ID3D11RenderTargetView : ID3D11View {};
struct ID3D11Texture2D {
    long refs = 1; DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM; unsigned bind = 0;
    void AddRef() { ++refs; }
    void Release() { --refs; ++g_texReleased; }
    // D3D11's GetDesc returns NOTHING (D3D12's returns HRESULT): the stub says so too, which is what
    // makes "if (FAILED(tex->GetDesc(...)))" a compile error here as well as in the real build.
    void GetDesc(D3D11_TEXTURE2D_DESC* d) const { d->Format = fmt; d->BindFlags = bind; }
};
struct ID3D11Device {
    HRESULT CreateShaderResourceView(ID3D11Texture2D* t, void*, ID3D11ShaderResourceView** out) {
        if (!(t->bind & D3D11_BIND_SHADER_RESOURCE)) { *out = nullptr; return E_FAIL; }
        *out = new ID3D11ShaderResourceView(); ++g_srvCreated; return S_OK;
    }
    HRESULT CreateRenderTargetView(ID3D11Texture2D* t, const D3D11_RENDER_TARGET_VIEW_DESC* d,
                                   ID3D11RenderTargetView** out) {
        if (!(t->bind & D3D11_BIND_RENDER_TARGET)) { *out = nullptr; return E_FAIL; }
        if (d && d->Format != t->fmt && t->fmt != DXGI_FORMAT_B8G8R8A8_TYPELESS) {
            *out = nullptr; return E_FAIL;   // an sRGB view over UNORM bytes is refused, as D3D11 does
        }
        *out = new ID3D11RenderTargetView(); ++g_rtvCreated; return S_OK;
    }
};
static ID3D11Device* g_d3d11 = nullptr;
static bool g_eyeFmtSrgb = true;
'''

cache = XR[XR.index('enum { kViewCacheSlots = 24 };'):]
cache = cache[:cache.index('static inline ID3D11RenderTargetView* CachedRtv')]
cache += block('static inline ID3D11RenderTargetView* CachedRtv')   # the whole function, signature too

direct = block('static DXGI_FORMAT OlrDirectViewFormat(ID3D11Texture2D* tex, bool* srgbOut)')

checks = r'''
int main() {
    ID3D11Device dev; g_d3d11 = &dev;

    // ---- the view cache ---------------------------------------------------------------
    ID3D11Texture2D swap[3];
    for (int i = 0; i < 3; ++i) {
        swap[i].fmt = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;   // what an eye swapchain image really is
        swap[i].bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    }

    // One view per texture, reused: 60 frames x 2 eyes must not allocate 120 views.
    for (int frame = 0; frame < 60; ++frame)
        for (int e = 0; e < 2; ++e) assert(CachedSrv(&swap[e]) != nullptr);
    assert(g_srvCreated == 2);
    assert(g_viewHits == 118 && g_viewMisses == 2);
    assert(CachedSrv(&swap[0]) == CachedSrv(&swap[0]));

    // The entry holds its texture, so the address of one that is freed cannot be handed to a new
    // texture while the entry is alive - the exact failure the old code refused to risk by not caching.
    assert(swap[0].refs == 2);   // ours + the cache

    // A render-target view is keyed by its format as well. On a non-typeless image D3D11 only allows
    // the image's OWN format, and the cache passes that refusal through rather than handing back the
    // view of the other colour space - which is precisely the failure B8 exists to avoid.
    ID3D11RenderTargetView* a = CachedRtv(&swap[0], DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    assert(a != nullptr && g_rtvCreated == 1);
    assert(CachedRtv(&swap[0], DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) == a);
    assert(CachedRtv(&swap[0], DXGI_FORMAT_B8G8R8A8_UNORM) == nullptr);
    // On a typeless image both are legal, and they must be two separate entries.
    ID3D11Texture2D tl; tl.fmt = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    tl.bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ID3D11RenderTargetView* u = CachedRtv(&tl, DXGI_FORMAT_B8G8R8A8_UNORM);
    ID3D11RenderTargetView* s = CachedRtv(&tl, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    assert(u && s && u != s && g_rtvCreated == 3);

    // A texture that cannot carry a view is answered with nullptr, never with something else.
    ID3D11Texture2D plain; plain.bind = 0;
    assert(CachedSrv(&plain) == nullptr);

    // Clearing releases everything it owned and starts over: that is the rebuild path.
    const int entries = g_viewN;
    const int viewsBefore = g_viewReleased, texBefore = g_texReleased;
    assert(entries == 5);
    ViewCacheClear();
    assert(g_viewN == 0);
    assert(g_viewReleased == viewsBefore + entries && g_texReleased == texBefore + entries);
    assert(CachedSrv(&swap[1]) != nullptr);

    // More live textures than slots cannot grow the table without bound.
    ID3D11Texture2D many[kViewCacheSlots + 5];
    for (int i = 0; i < kViewCacheSlots + 5; ++i) {
        many[i].bind = D3D11_BIND_SHADER_RESOURCE;
        assert(CachedSrv(&many[i]) != nullptr);
        assert(g_viewN <= kViewCacheSlots);
    }

    // [Perf] ViewCache=0 is the OLD shape: a view made for every call and dropped by the next one, so
    // the switch really can take the cache out of the picture.
    g_viewCacheOn = 0;
    const int churn = g_srvCreated;
    for (int i = 0; i < 10; ++i) assert(CachedSrv(&swap[0]) != nullptr);
    assert(g_srvCreated == churn + 10);
    assert(g_viewN == 1);   // one alive at a time, exactly like the create/release pair it replaces
    g_viewCacheOn = 1;
    const int kept = g_srvCreated;
    for (int i = 0; i < 10; ++i) assert(CachedSrv(&swap[0]) != nullptr);
    assert(g_srvCreated == kept);        // the cached view is reused, nothing is made
    ViewCacheClear();                    // a swapchain rebuild
    for (int i = 0; i < 10; ++i) assert(CachedSrv(&swap[0]) != nullptr);
    assert(g_srvCreated == kept + 1);    // exactly one new view after the rebuild, then reused again

    // ---- B8: the view a direct draw is allowed to use ---------------------------------
    bool srgb = false;
    ID3D11Texture2D dst; dst.bind = D3D11_BIND_RENDER_TARGET;
    dst.fmt = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    assert(OlrDirectViewFormat(&dst, &srgb) == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && srgb);
    dst.fmt = DXGI_FORMAT_B8G8R8A8_UNORM;      // a runtime that substituted the format
    assert(OlrDirectViewFormat(&dst, &srgb) == DXGI_FORMAT_B8G8R8A8_UNORM && !srgb);
    dst.fmt = DXGI_FORMAT_B8G8R8A8_TYPELESS;   // typeless: an explicit format is mandatory
    g_eyeFmtSrgb = true;
    assert(OlrDirectViewFormat(&dst, &srgb) == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && srgb);
    g_eyeFmtSrgb = false;
    assert(OlrDirectViewFormat(&dst, &srgb) == DXGI_FORMAT_B8G8R8A8_UNORM && !srgb);
    dst.fmt = DXGI_FORMAT_R8G8B8A8_UNORM;      // not this family: keep the proved copy path
    assert(OlrDirectViewFormat(&dst, &srgb) == DXGI_FORMAT_UNKNOWN);
    dst.fmt = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    dst.bind = 0;                              // not a render target at all
    assert(OlrDirectViewFormat(&dst, &srgb) == DXGI_FORMAT_UNKNOWN);
    assert(OlrDirectViewFormat(nullptr, &srgb) == DXGI_FORMAT_UNKNOWN);

    std::puts("ok");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as td:
    src = Path(td) / 't.cpp'
    exe = Path(td) / 't.exe'
    src.write_text(stub + cache + '\n' + direct + '\n' + checks)
    subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-o', str(exe), str(src)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    assert out.strip() == 'ok', out
print('view cache + direct view format: the shipped code, compiled and exercised')

# ------------------------------------------------- 4. B8: the copy only happens when it must
draw = block('static bool GammaDraw(')
assert 'directRtv = CachedRtv(dst, vf);' in draw, 'the direct draw no longer asks the cache for its view'
assert 'OlrDirectViewFormat(dst, &dstIsSrgb)' in draw, \
    'the direct draw stopped taking the colour space from the destination'
assert 'if (!directRtv) g_ctx->CopyResource(dst, tmp);' in draw, \
    'the copy is no longer skipped on the direct lane (or is skipped unconditionally)'
assert draw.index('if (!directRtv && !EnsureBlitTmp') < draw.index('CachedSrv(src)'), \
    'the scratch target is ensured after the source view is taken'
assert 'srv->Release();' not in draw, 'GammaDraw releases a view it does not own any more'

# The cache must be emptied BEFORE the swapchains it holds references to are destroyed: an entry holds
# its texture, and the runtime is entitled to free the images together with the swapchain.
destroy = block('static void DestroyEyeSwapchains()')
assert destroy.index('ViewCacheClear();') < destroy.index('destroySwapchain'), \
    'the view cache is dropped after the swapchains (a runtime could free an image under it)'
rebuild = block('static bool RebuildForSize(')
assert rebuild.index('ExDirectRelease();') < rebuild.index('destroySwapchain'), \
    'a rebuild keeps pointing the blits at the old backbuffer'
assert 'ViewCacheClear();' in rebuild, 'a rebuild keeps views on the destroyed backbuffer'
shutdown = block('void OLXR_Shutdown()')
assert 'ViewCacheClear();' in shutdown, 'shutdown leaks every cached view'
print('B8/A3: the copy is conditional, and every teardown drops the cache first')

print('\nall phase-4 checks passed')
