"""The interaction-profile bindings: Valve Index works, and the Meta Quest bindings do not move.

Checks the real suggestion tables in src/ol_xr.cpp against the component paths the OpenXR 1.1
interaction profiles allow (spec chapter "Interaction Profile Paths"), so a path no runtime has
(Valve's trackpad has no click), or one bound to the wrong hand, can never ship. Then it pins the
Oculus Touch table exactly, because those are the bindings every Quest player already uses: a fix
for another controller must not touch them. No headset needed - the source is the evidence.
Run: python3 tests/xr_valve_bindings_test.py
"""
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
xr = (ROOT / 'src/ol_xr.cpp').read_text()

SIMPLE = {'select/click', 'menu/click', 'grip/pose', 'aim/pose', 'output/haptic'}
TOUCH = {'x/click', 'x/touch', 'y/click', 'y/touch', 'menu/click',
         'a/click', 'a/touch', 'b/click', 'b/touch', 'system/click',
         'squeeze/value', 'trigger/value', 'trigger/touch', 'trigger/proximity',
         'thumbstick/x', 'thumbstick/y', 'thumbstick/click', 'thumbstick/touch',
         'thumbrest/touch', 'thumb_resting_surfaces/proximity',
         'grip/pose', 'aim/pose', 'output/haptic'}
INDEX = {'system/click', 'system/touch', 'a/click', 'a/touch', 'b/click', 'b/touch',
         'squeeze/value', 'squeeze/force', 'trigger/click', 'trigger/value', 'trigger/touch',
         'thumbstick/x', 'thumbstick/y', 'thumbstick/click', 'thumbstick/touch',
         'trackpad/x', 'trackpad/y', 'trackpad/force', 'trackpad/touch',
         'grip/pose', 'aim/pose', 'output/haptic'}
WMR = {'menu/click', 'squeeze/click', 'trigger/value', 'thumbstick/x', 'thumbstick/y',
       'thumbstick/click', 'trackpad/x', 'trackpad/y', 'trackpad/click', 'trackpad/touch',
       'grip/pose', 'aim/pose', 'output/haptic'}
VIVE = {'system/click', 'squeeze/click', 'menu/click', 'trigger/click', 'trigger/value',
        'trackpad/x', 'trackpad/y', 'trackpad/click', 'trackpad/touch',
        'grip/pose', 'aim/pose', 'output/haptic'}
ALLOW = {
    '/interaction_profiles/khr/simple_controller': SIMPLE,
    '/interaction_profiles/oculus/touch_controller': TOUCH,
    '/interaction_profiles/valve/index_controller': INDEX,
    '/interaction_profiles/microsoft/motion_controller': WMR,
    '/interaction_profiles/htc/vive_controller': VIVE,
}
# A Vector2 action binds the component itself; that composite path is valid for every profile whose
# allowlist has the component's x/y.
COMPOSITES = {'thumbstick', 'trackpad'}
# Components that exist on one hand only. The Index has the system button on the left; Touch has X/Y
# and Menu on the left and A/B and System on the right.
HAND_ONLY = {
    '/interaction_profiles/oculus/touch_controller': (
        {'x/click', 'x/touch', 'y/click', 'y/touch', 'menu/click'},
        {'a/click', 'a/touch', 'b/click', 'b/touch', 'system/click'}),
    '/interaction_profiles/valve/index_controller': ({'system/click', 'system/touch'}, set()),
}

# ---- the tables, straight out of the source ------------------------------------------------
TABLES = {}
for name in ('touch', 'index', 'wmr', 'vive', 'simple'):
    body = re.search(r'const Bind %s\[\] = \{(.*?)\n    \};' % name, xr, re.S).group(1)
    pairs = re.findall(r'\{\s*(g_a\w+)\s*,\s*"([^"]+)"\s*\}', body)
    profile = re.search(r'suggest\("([^"]+)",\s*%s,' % name, xr).group(1)
    TABLES[profile] = pairs
assert len(TABLES) == 5, 'all five profiles are still suggested'
assert set(TABLES) == set(ALLOW), 'a profile was added or removed: update this test with its allowlist'

for profile, pairs in TABLES.items():
    seen = set()
    for action, path in pairs:
        assert path.startswith('/user/hand/left/') or path.startswith('/user/hand/right/'), (profile, path)
        assert (action, path) not in seen, 'duplicate binding %s in %s' % (path, profile)
        seen.add((action, path))
        hand = 'left' if '/left/' in path else 'right'
        comp = 'output/haptic' if '/output/' in path else path.split('/input/', 1)[1]
        ok = comp in ALLOW[profile] or (comp in COMPOSITES and (comp + '/x') in ALLOW[profile])
        assert ok, '%s is not a path of %s (OpenXR interaction profiles)' % (comp, profile)
        leftOnly, rightOnly = HAND_ONLY.get(profile, (set(), set()))
        assert not (hand == 'right' and comp in leftOnly), '%s is a LEFT hand path (%s)' % (comp, profile)
        assert not (hand == 'left' and comp in rightOnly), '%s is a RIGHT hand path (%s)' % (comp, profile)
    # Every profile must be able to walk, lean, grab and be seen.
    comps = {p.split('/input/', 1)[1] if '/input/' in p else 'output/haptic' for _, p in pairs}
    for need in ('grip/pose', 'aim/pose', 'output/haptic'):
        assert need in comps, '%s does not bind %s' % (profile, need)
    assert comps & {'trigger/value', 'select/click'}, '%s has no trigger: no analog lean' % profile
    assert comps & {'thumbstick', 'thumbstick/x', 'trackpad', 'trackpad/x', 'select/click'}, \
        '%s has no stick-like input: Miles cannot walk' % profile
    if profile != '/interaction_profiles/khr/simple_controller':   # the fallback has nothing else
        assert comps & {'squeeze/value', 'squeeze/click'}, '%s has no grip' % profile


# ---- the Valve Index: all four face buttons, and a menu an app can actually read ------------
idx = set(TABLES['/interaction_profiles/valve/index_controller'])
for want in (('g_aPrimary', '/user/hand/right/input/a/click'),      # -> A: jump / vault
             ('g_aSecondary', '/user/hand/right/input/b/click'),    # -> B: use / open a door
             ('g_aPrimary', '/user/hand/left/input/a/click'),       # -> X: crouch
             ('g_aSecondary', '/user/hand/left/input/b/click'),     # -> Y: reload
             ('g_aMove', '/user/hand/left/input/thumbstick'),       # walk
             ('g_aMove', '/user/hand/right/input/thumbstick'),      # turn
             ('g_aGrip', '/user/hand/left/input/squeeze/value'),    # sprint / hold on
             ('g_aGrip', '/user/hand/right/input/squeeze/value'),   # camcorder / hold on
             ('g_aMenu', '/user/hand/left/input/system/click'),     # the runtime may never deliver it
             ('g_aIndexForce', '/user/hand/left/input/trackpad/force')):   # ...so this is the menu
    assert want in idx, 'the Valve Index profile must bind %s to %s' % (want[0], want[1])

# ---- the Meta Quest table is exactly what shipped: no fix elsewhere may move it -------------
EXPECTED_TOUCH = {
    ('g_aPose', '/user/hand/left/input/grip/pose'), ('g_aPose', '/user/hand/right/input/grip/pose'),
    ('g_aAim', '/user/hand/left/input/aim/pose'), ('g_aAim', '/user/hand/right/input/aim/pose'),
    ('g_aMove', '/user/hand/left/input/thumbstick'), ('g_aMove', '/user/hand/right/input/thumbstick'),
    ('g_aClick', '/user/hand/left/input/thumbstick/click'), ('g_aClick', '/user/hand/right/input/thumbstick/click'),
    ('g_aTrig', '/user/hand/left/input/trigger/value'), ('g_aTrig', '/user/hand/right/input/trigger/value'),
    ('g_aGrip', '/user/hand/left/input/squeeze/value'), ('g_aGrip', '/user/hand/right/input/squeeze/value'),
    ('g_aPrimary', '/user/hand/left/input/x/click'), ('g_aPrimary', '/user/hand/right/input/a/click'),
    ('g_aSecondary', '/user/hand/left/input/y/click'), ('g_aSecondary', '/user/hand/right/input/b/click'),
    ('g_aMenu', '/user/hand/left/input/menu/click'),
    ('g_aHaptic', '/user/hand/left/output/haptic'), ('g_aHaptic', '/user/hand/right/output/haptic'),
    ('g_aStickTouch', '/user/hand/left/input/thumbstick/touch'),
    ('g_aStickTouch', '/user/hand/right/input/thumbstick/touch'),
}
assert set(TABLES['/interaction_profiles/oculus/touch_controller']) == EXPECTED_TOUCH, \
    'the Oculus Touch bindings changed - every Quest player uses these'

# ---- one refused path must not take a whole profile down -----------------------------------
# OpenXR: a suggested binding outside the profile's allowlist makes xrSuggestInteractionProfileBindings
# return XR_ERROR_PATH_UNSUPPORTED for the WHOLE call. So the paths a runtime may refuse, or may accept
# and never deliver, are suggested in a second pass without them.
opt = xr[xr.index('auto optional = [&](const Bind& b)'):xr.index('auto suggest = [&]')]
for must in ('b.a == g_aHaptic', 'b.a == g_aStickTouch', 'b.a == g_aIndexForce', '/input/system/'):
    assert must in opt, 'the optional set must cover %s' % must
suggest = xr[xr.index('auto suggest = [&]'):xr.index('// Oculus Touch:')]
assert 'for (int pass = 0; pass < 2; ++pass)' in suggest, 'a refused profile must be retried'
assert 'if (pass && optional(b[i])) continue;' in suggest, 'the retry drops only the optional paths'
assert 'if (XrSucceeded(rr)) return;' in suggest, 'the retry stops as soon as a pass is accepted'

# ---- and the Index menu is read into the pad ----------------------------------------------
men = xr[xr.index('if (g_aIndexForce) {'):xr.index('if (g_aIndexForce) {') + 400]
assert 'p.buttons |= OLXR_BTN_MENU' in men, 'the trackpad press must open the menu'
assert 'f > 0.40f' in men and 'f < 0.20f' in men, 'the press needs hysteresis so a resting thumb cannot pause'

# ---- the retry really retries: the shipped lambda, compiled and run -------------------------
# The text checks above can be satisfied by a comment. This compiles the real "optional" and "suggest"
# lambdas and runs them twice: against a fake runtime that refuses any set containing a system path -
# which is exactly what "xrSuggestInteractionProfileBindings returns XR_ERROR_PATH_UNSUPPORTED for the
# whole call" means - and against one that takes the full set. See the OpenXR input chapter.
lambda_src = xr[xr.index('struct Bind { XrAction a; const char* path; };'):xr.index('    // Oculus Touch:')]
HARNESS_HEAD = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using XrPath = unsigned long long;
using XrAction = unsigned long long;
using XrResult = int;
using XrInstance = unsigned long long;
using uint32_t = unsigned;
static constexpr unsigned XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING_VALUE = 44;
static constexpr int XR_SUCCESS = 0, XR_ERROR_PATH_UNSUPPORTED = -9;
static bool XrSucceeded(XrResult r) { return r >= 0; }
struct XrActionSuggestedBinding { XrAction action; XrPath binding; };
struct XrInteractionProfileSuggestedBinding {
    unsigned type; const void* next; XrPath interactionProfile;
    uint32_t countSuggestedBindings; const XrActionSuggestedBinding* suggestedBindings;
};
static void XLOG(const char*, ...) {}
static XrAction g_aHaptic = 10, g_aStickTouch = 11, g_aIndexForce = 12;
static XrInstance g_instance = 1;
static std::vector<std::vector<XrPath> > g_calls;
static bool RefusesSystemPaths = true;
static XrPath XrPathOf(const char* s) {
    if (!s || !*s) return 0;
    unsigned h = 2166136261u;
    for (const char* c = s; *c; ++c) h = (h ^ (unsigned char)*c) * 16777619u;
    return (XrPath)h + 1000;
}
static XrResult FakeSuggest(XrInstance, const XrInteractionProfileSuggestedBinding* b) {
    std::vector<XrPath> paths;
    for (uint32_t i = 0; i < b->countSuggestedBindings; ++i) paths.push_back(b->suggestedBindings[i].binding);
    g_calls.push_back(paths);
    if (!RefusesSystemPaths) return XR_SUCCESS;
    for (size_t i = 0; i < paths.size(); ++i)
        if (paths[i] == XrPathOf("/user/hand/left/input/system/click")) return XR_ERROR_PATH_UNSUPPORTED;
    return XR_SUCCESS;
}
struct Fn { XrResult (*suggestInteractionProfileBindings)(XrInstance, const XrInteractionProfileSuggestedBinding*); };
static Fn g_fn = { FakeSuggest };
'''
HARNESS_CALL = r'''
    const Bind idx[] = {
        { (XrAction)1, "/user/hand/left/input/grip/pose" },
        { (XrAction)2, "/user/hand/left/input/system/click" },
        { g_aIndexForce, "/user/hand/left/input/trackpad/force" },
        { g_aHaptic,    "/user/hand/left/output/haptic" },
    };
    suggest("/interaction_profiles/valve/index_controller", idx, 4);
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Run(true);
    assert(g_calls.size() == 2);      // refused, then retried
    assert(g_calls[0].size() == 4);   // the full set was offered first
    assert(g_calls[1].size() == 1);   // the retry keeps the pose and drops system / trackpad force / haptics
    assert(g_calls[1][0] == XrPathOf("/user/hand/left/input/grip/pose"));
    Run(false);
    assert(g_calls.size() == 1);      // a runtime that takes the full set is asked exactly once
    std::printf("Retry: a refused profile is re-suggested without the paths a runtime may refuse.\n");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'suggest.cpp'
    exe = Path(tmp) / 'suggest-test'
    func_head = 'static void Run(bool refuses) {\n    g_calls.clear(); RefusesSystemPaths = refuses;\n'
    src.write_text(HARNESS_HEAD + func_head + lambda_src + HARNESS_CALL)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

print('Valve Index: every bound path is a real OpenXR Index path, all four face buttons, a usable menu,')
print('             one refused path cannot drop the profile, and the Quest bindings are untouched.')
