"""Runtime-facing maths: the overscan fill, the runtime-FOV decision, the pose lead and the
runtime-recenter fold.

Two separate reports live in here. (1) The black bars at the edge of view during a fast head turn
are the runtime's own reprojection: it warps the frame this PC sent to where the head is at
scan-out, and whatever was never rendered comes out black. (2) A recenter done in the HEADSET's own
menu (SteamVR dashboard, Oculus "Reset View") used to move this mod's world, because the mod's
baseline was captured once and the runtime had silently redefined its origin.

All four pieces are pure arithmetic, so they are compiled here from the real source and checked
without a headset:

  OLR_EdgeFillBox()             the blit rect that stretches the picture's edge over the margin
  OLR_DecideFitFrustum()        auto/off/on, driven by the runtime's fovMutable flag
  OLR_ClampPoseLeadPeriods()    bounds on [Stream] PoseLeadPeriods
  OLR_ReanchorOnSpaceChange()   the runtime-recenter fold, checked against its invariance property

Does not replace a live run on SteamVR / Quest Link: it proves the numbers, not the compositor.
Run: python3 tests/stream_edge_fill_test.py
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/ol_xr.cpp').read_text()
ini = (ROOT / 'runtime/outlastvr.ini').read_text()
types = (ROOT / 'src/ol_xr_types.h').read_text()


def block(signature):
    """The whole function, from its signature to its matching closing brace."""
    start = source.index(signature)
    i = source.index('{', start)
    depth = 0
    while i < len(source):
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
        i += 1
    raise AssertionError('unbalanced function: ' + signature)


# The real quaternion helpers, so the re-anchor check is the code that ships, not a re-implementation.
funcs = (block('static const float kEdgeFillMinCoverage')      # the constant, then OLR_EdgeFillBox
         + '\n' + block('static int OLR_DecideFitFrustum')
         + '\n' + block('static int OLR_ClampPoseLeadPeriods')
         + '\n' + block('static XrQuaternionf QNormalize(XrQuaternionf q) {')
         + '\n' + block('static XrQuaternionf QConj(XrQuaternionf q) {')
         + '\n' + block('static XrQuaternionf QMul(XrQuaternionf a, XrQuaternionf b) {')
         + '\n' + block('static XrVector3f QRotate(XrQuaternionf q, XrVector3f v) {')
         + '\n' + block('static void OLR_ReanchorOnSpaceChange')
         + '\n' + block('static float SrgbToLinear')
         + '\n' + block('static float SrgbFromLinear')
         + '\n' + block('static bool OLR_FormatIsSrgb')
         + '\n' + block('static int64_t OLR_PickEyeFormat'))

stubs = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
// The three OpenXR types the helpers touch (field-for-field from src/ol_xr_types.h).
struct XrQuaternionf { float x, y, z, w; };
struct XrVector3f { float x, y, z; };
struct XrPosef { XrQuaternionf orientation; XrVector3f position; };
// The two DXGI formats the negotiation may choose between (real DXGI values).
#define DXGI_FORMAT_B8G8R8A8_UNORM      87
#define DXGI_FORMAT_B8G8R8A8_UNORM_SRGB 91
'''

checks = r'''
static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

// The affine map the fill produces: destination x -> source x.
static float mapX(float dx, float sX, float kx) { return sX + dx * kx; }

int main() {
    float sX = 0, sY = 0, sW = 0, sH = 0;
    // -- the case that matters: the game rendered less than the lens covers -------------------
    // Eye 1000x1000. The picture covers 100..900 horizontally, 50..950 vertically; it comes from
    // source columns 0..640 and rows 0..900 of a 1920x1080 frame.
    assert(OLR_EdgeFillBox(100.0f, 50.0f, 900.0f, 950.0f,
                           0.0f, 0.0f, 640.0f, 900.0f, 1000.0f, 1000.0f,
                           &sX, &sY, &sW, &sH));
    const float kx = 640.0f / 800.0f, ky = 900.0f / 900.0f;
    assert(near(kx, 0.8f) && near(ky, 1.0f));
    // The extended box must start where the eye image starts and cover all of it.
    assert(near(sX, 0.0f - 100.0f * kx));
    assert(near(sW, 1000.0f * kx));
    assert(near(sY, 0.0f - 50.0f * ky));
    assert(near(sH, 1000.0f * ky));
    // And it must be the SAME map as before inside the picture: a pixel at the picture's left
    // edge still samples source column 0, the right edge column 640, the centre the centre.
    // That identity is the whole safety argument for the fill.
    assert(near(mapX(100.0f, sX, kx), 0.0f));
    assert(near(mapX(900.0f, sX, kx), 640.0f));
    assert(near(mapX(500.0f, sX, kx), 320.0f));
    // Outside the picture the map runs past the source on purpose: the shader's clamp and the
    // CLAMP sampler then repeat the edge texels instead of reading black.
    assert(mapX(0.0f, sX, kx) < 0.0f);
    assert(mapX(1000.0f, sX, kx) > 640.0f);

    // -- a render that already covers the whole eye: the fill changes nothing ----------------
    assert(OLR_EdgeFillBox(0.0f, 0.0f, 1000.0f, 1000.0f,
                           0.0f, 0.0f, 800.0f, 1000.0f, 1000.0f, 1000.0f,
                           &sX, &sY, &sW, &sH));
    assert(near(sX, 0.0f) && near(sW, 800.0f) && near(sY, 0.0f) && near(sH, 1000.0f));

    // -- degenerate geometry never reaches the blit as a NaN or a negative box --------------
    assert(!OLR_EdgeFillBox(100.0f, 0.0f, 100.0f, 100.0f, 0, 0, 10, 10, 100, 100, &sX, &sY, &sW, &sH));
    assert(!OLR_EdgeFillBox(0.0f, 0.0f, 100.0f, 0.0f, 0, 0, 10, 10, 100, 100, &sX, &sY, &sW, &sH));
    assert(!OLR_EdgeFillBox(0.0f, 0.0f, 100.0f, 100.0f, 0, 0, 10, 10, 0.0f, 100, &sX, &sY, &sW, &sH));
    assert(!OLR_EdgeFillBox(0.0f, 0.0f, 100.0f, 100.0f, 50, 50, 50, 50, 100, 100, &sX, &sY, &sW, &sH));
    float* nul = nullptr;
    assert(!OLR_EdgeFillBox(0.0f, 0.0f, 100.0f, 100.0f, 0, 0, 10, 10, 100, 100, nul, &sY, &sW, &sH));

    // -- a deliberately narrow frame (a menu, a scripted camera) keeps its honest black field -
    // 58 deg rendered inside a 100 deg lens: stretching that edge would be a smear, not overscan.
    assert(!OLR_EdgeFillBox(210.0f, 210.0f, 790.0f, 790.0f, 0, 0, 640, 900, 1000.0f, 1000.0f,
                            &sX, &sY, &sW, &sH));
    // Just over three quarters of the eye is gameplay's worst case and IS filled.
    assert(OLR_EdgeFillBox(120.0f, 120.0f, 880.0f, 880.0f, 0, 0, 640, 900, 1000.0f, 1000.0f,
                           &sX, &sY, &sW, &sH));

    // -- which runtime gets the pre-warp ----------------------------------------------------
    // AUTO, the runtime answered: mutable means the declared FOV is honoured (SteamVR, VDXR).
    assert(OLR_DecideFitFrustum(-1, false, true, true) == 0);
    // AUTO, the runtime answered "not mutable": pre-warp into its frustum (Quest Link/Air Link).
    assert(OLR_DecideFitFrustum(-1, false, true, false) == 1);
    // The flag outranks the name, both ways.
    assert(OLR_DecideFitFrustum(-1, true, true, true) == 0);
    assert(OLR_DecideFitFrustum(-1, false, true, false) == 1);
    // No answer from the runtime: the runtime-name fallback, exactly as every earlier build.
    assert(OLR_DecideFitFrustum(-1, true, false, true) == 1);
    assert(OLR_DecideFitFrustum(-1, false, false, false) == 0);
    // A forced ini value always wins, in both directions.
    assert(OLR_DecideFitFrustum(1, false, true, true) == 1);
    assert(OLR_DecideFitFrustum(0, true, true, false) == 0);

    // -- a runtime recenter must not move the player's world --------------------------------
    // P = the pose of the NEW origin expressed in the OLD space, exactly what
    // XrEventDataReferenceSpaceChangePending carries: 35 deg of yaw and a 1.2 m shift.
    XrPosef P;
    const float half = 35.0f * 3.14159265f / 180.0f * 0.5f;
    P.orientation = { 0.0f, std::sin(half), 0.0f, std::cos(half) };
    P.position = { 1.2f, -0.3f, 0.8f };
    // The baseline the mod captured before the runtime recentred (20 deg yaw, 1.6 m up).
    const XrQuaternionf qOld = { 0.0f, std::sin(10.0f * 3.14159265f / 180.0f), 0.0f,
                                     std::cos(10.0f * 3.14159265f / 180.0f) };
    const XrVector3f pOld = { 0.1f, 1.6f, -0.2f };
    // A head pose, given in the NEW space, and the same pose seen from the OLD space. The
    // ORIENTATION transforms too (q_old = qP * q_new) - getting that wrong is the classic way to
    // "prove" a re-anchor works while the picture spins.
    const XrQuaternionf qHeadNew = { 0.15f, 0.0f, 0.05f, 0.987f };
    const XrQuaternionf qHeadOld = QMul(P.orientation, qHeadNew);
    const XrVector3f cNew = { 0.3f, 1.7f, 0.1f };
    XrVector3f cOld = QRotate(P.orientation, cNew);
    cOld.x += P.position.x; cOld.y += P.position.y; cOld.z += P.position.z;

    // Before: the relative pose the mod renders with, from the old baseline in the old space.
    const XrQuaternionf relQOld = QMul(QConj(qOld), qHeadOld);
    const XrVector3f relPOld = QRotate(QConj(qOld), { cOld.x - pOld.x, cOld.y - pOld.y, cOld.z - pOld.z });

    // Fold the runtime's change into the baseline, then compute the SAME relative pose again.
    XrQuaternionf qBase = qOld;
    XrVector3f pBase = pOld;
    OLR_ReanchorOnSpaceChange(&qBase, &pBase, P);
    const XrQuaternionf relQNew = QMul(QConj(qBase), qHeadNew);
    const XrVector3f relPNew = QRotate(QConj(qBase), { cNew.x - pBase.x, cNew.y - pBase.y, cNew.z - pBase.z });

    // The whole point: unchanged. (0.1 mm / 0.01 deg is float noise, a real error would be metres.)
    assert(std::fabs(relPNew.x - relPOld.x) < 1e-4f);
    assert(std::fabs(relPNew.y - relPOld.y) < 1e-4f);
    assert(std::fabs(relPNew.z - relPOld.z) < 1e-4f);
    {
        const float d = std::fabs(relQNew.x * relQOld.x + relQNew.y * relQOld.y +
                                  relQNew.z * relQOld.z + relQNew.w * relQOld.w);
        assert(d > 0.99999f);              // same orientation (dot of unit quaternions, either sign)
    }

    // A runtime recenter that changes nothing leaves the baseline exactly alone.
    XrQuaternionf qId = qOld; XrVector3f pId = pOld;
    XrPosef none = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
    OLR_ReanchorOnSpaceChange(&qId, &pId, none);
    assert(std::fabs(qId.y - qOld.y) < 1e-6f && std::fabs(qId.w - qOld.w) < 1e-6f);
    assert(std::fabs(pId.x - pOld.x) < 1e-6f && std::fabs(pId.z - pOld.z) < 1e-6f);

    // A pure 90 deg yaw recenter must rotate the baseline by the INVERSE of that yaw, and must not
    // touch a baseline at the origin.
    XrQuaternionf qY = { 0, 0, 0, 1 }; XrVector3f pY = { 0, 0, 0 };
    const float h45 = 90.0f * 3.14159265f / 180.0f * 0.5f;
    XrPosef yawOnly = { { 0.0f, std::sin(h45), 0.0f, std::cos(h45) }, { 0, 0, 0 } };
    OLR_ReanchorOnSpaceChange(&qY, &pY, yawOnly);
    assert(std::fabs(qY.y + std::sin(h45)) < 1e-5f && std::fabs(qY.w - std::cos(h45)) < 1e-5f);
    assert(std::fabs(pY.x) < 1e-6f && std::fabs(pY.y) < 1e-6f && std::fabs(pY.z) < 1e-6f);

    // Null pointers must be survivable (the helper is called with the live baseline only, but a
    // refactor must not be able to crash the render thread).
    OLR_ReanchorOnSpaceChange(nullptr, nullptr, P);

    // -- the eye-image format asked of the runtime -------------------------------------------
    // The sRGB transfer and its inverse have to be exact inverses, or a runtime without an sRGB
    // swapchain would show a picture that is one gamma step off - the whole risk of that path.
    const float probes[5] = { 0.0f, 0.004f, 0.18f, 0.5f, 1.0f };
    for (int i = 0; i < 5; ++i) {
        const float roundTrip = SrgbFromLinear(SrgbToLinear(probes[i]));
        assert(std::fabs(roundTrip - probes[i]) < 1e-4f);
    }
    assert(SrgbFromLinear(-1.0f) == 0.0f && SrgbFromLinear(2.0f) == 1.0f);
    assert(std::fabs(SrgbFromLinear(0.5f) - 0.7354f) < 0.001f);   // the sRGB knee, by definition

    // Asking for what the runtime offers: no change at all (every runtime this mod has run on).
    const int64_t offered[6] = { 28, 91, 87, 10, 2, 29 };
    assert(OLR_PickEyeFormat(offered, 6, 91) == 91);
    // sRGB not offered, the byte-identical format is: take it instead of letting it substitute.
    const int64_t noSrgb[4] = { 87, 28, 10, 2 };
    assert(OLR_PickEyeFormat(noSrgb, 4, 91) == 87);
    // Neither offered: keep the request (the runtime substitutes, as every earlier build did).
    const int64_t neitherFmt[3] = { 28, 10, 2 };
    assert(OLR_PickEyeFormat(neitherFmt, 3, 91) == 91);
    // A runtime that will not answer is not a reason to change anything.
    assert(OLR_PickEyeFormat(nullptr, 4, 91) == 91);
    assert(OLR_PickEyeFormat(offered, 0, 91) == 91);
    // A non-sRGB request is never silently swapped for the sRGB one either.
    assert(OLR_PickEyeFormat(noSrgb, 4, 87) == 87);
    // And which of the two needs the pre-encoded LUT.
    assert(OLR_FormatIsSrgb(91) == true);
    assert(OLR_FormatIsSrgb(87) == false);

    // -- the pose lead knob ------------------------------------------------------------------
    assert(OLR_ClampPoseLeadPeriods(-5) == 0);
    assert(OLR_ClampPoseLeadPeriods(0) == 0);
    assert(OLR_ClampPoseLeadPeriods(1) == 1);     // the shipped default
    assert(OLR_ClampPoseLeadPeriods(2) == 2);
    assert(OLR_ClampPoseLeadPeriods(3) == 2);
    assert(OLR_ClampPoseLeadPeriods(9999) == 2);
    std::printf("Stream fill / runtime-FOV / pose-lead maths passed.\n");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp) / 'stream.cpp'
    exe = Path(tmp) / 'stream-test'
    cpp.write_text(stubs + funcs + checks)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# ---- the wiring: none of the maths matters if the source never calls it ---------------------
assert 'static bool g_edgeFill = true;' in source, 'edge fill must default ON'
assert 'GetPrivateProfileIntW(L"Stream", L"EdgeFill", 1, ini)' in source, '[Stream] EdgeFill must be read'
assert 'GetPrivateProfileStringW(L"Stream", L"PoseLeadPeriods", L"1", pl, 32, ini)' in source, \
    '[Stream] PoseLeadPeriods must be read'
assert 'g_edgeFill && OLR_EdgeFillBox(' in source, 'the fitted blit must use the fill'
assert 'kEdgeFillMinCoverage = 0.75f' in source, 'the narrow-frame cutoff is part of the contract'
assert 'OLR_DecideFitFrustum(g_fitMode, g_isOculusRuntime, g_fovMutableKnown, g_fovMutable)' in source, \
    'the fit decision must come from the shared helper'
assert 'vli.displayTime = fs.predictedDisplayTime + leadNs;' in source, 'the pose lead must be applied'
assert 'g_poseLeadPeriods > 0 && fs.predictedDisplayPeriod > 0' in source, 'the lead must be period-based'
assert '[OLVR][STREAM] the game is handing over' in source, 'the refresh/fps hint must be logged'
assert 'RESOPT("xrGetViewConfigurationProperties", getViewConfigurationProperties)' in source, \
    'the fov flag must be optional, so an older loader cannot cost the session'
assert 'XR_TYPE_VIEW_CONFIGURATION_PROPERTIES_VALUE = 45' in types, 'wrong XrStructureType value'
assert 'XrBool32 fovMutable;' in types, 'XrViewConfigurationProperties must match the OpenXR ABI'

# ---- [REFCENTER]: a recenter done in the headset's own menu must not move the world ---------
assert 'XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING_VALUE = 40' in types, 'wrong event type value'
assert 'XrEventDataReferenceSpaceChangePending {' in types, 'the event struct must exist'
assert 'ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING_VALUE' in source, \
    'PollEvents must not drop the runtime recenter any more'
assert 'g_refChangePend && fs.predictedDisplayTime >= g_refChangeAt' in source, \
    'the fold must wait for the changeTime the runtime named'
assert 'OLR_ReanchorOnSpaceChange(&g_baseQ, &g_baseP, g_refChangePose)' in source, \
    'the frame loop must fold the change into the live baseline'
assert 'g_recenterReq = 1; g_centered = 0;' in source, \
    'a recenter with no pose must fall back to a plain re-zero'
assert '[OLVR][REFCENTER]' in source, 'the runtime recenter must be logged'

# ---- [SWAPFMT]: which format the runtime prefers, so a per-runtime difference is diagnosable --
assert 'enumerateSwapchainFormats(g_session, 0, &nf, nullptr)' in source, \
    'the runtime format list must be queried at session creation'
assert '[OLVR][SWAPFMT]' in source, 'the format list must be logged'
# ...and acted on, which is what makes it more than a diagnostic:
assert 'ci.format = g_eyeFmt;' in source, 'the eye swapchains must be created in the chosen format'
assert source.count(', g_eyeFmtSrgb);') == 3, 'all three eye blits must pass the destination format'
assert 'EnsureBlitTmp(dstW, dstH, dstSrgb)' in source, 'the scratch target must follow the destination'
assert 'ID3D11ShaderResourceView* srvs[2] = { srv, lut };' in source, 'the LUT must follow the destination'
assert 'g_lutSRVSrgbEnc' in source and 'SrgbFromLinear(lr)' in source, \
    'the pre-encoded LUT is what keeps a non-sRGB eye image from being one gamma step off'
assert 'GetPrivateProfileIntW(L"XR", L"RuntimeFormat", 1, ini)' in source, \
    'the escape hatch back to the shipped request must exist'
assert 'RuntimeFormat=1' in ini, 'RuntimeFormat must be documented in the shipped ini'

# ---- the shipped ini: documented, on, and matching the code defaults -------------------------
section = ini[ini.index('[Stream]'):]
assert 'EdgeFill=1' in section, 'EdgeFill ships ON'
assert 'PoseLeadPeriods=1' in section, 'PoseLeadPeriods ships at the shipped behaviour'
assert 'docs/STREAMING.md' in section, 'the section must point at the explanation'
print('Runtimes: overscan fill, spec-driven fit, pose lead, runtime recenter and eye-format negotiation verified.')
