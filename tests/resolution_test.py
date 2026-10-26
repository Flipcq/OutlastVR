"""Exercise actual resolution/config functions with synthetic Windows ini/log APIs.
Run: python3 tests/resolution_test.py (g++ required).
Does not exercise D3D9 resizing, WinForms, or a real OpenXR runtime.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/ol_ue3.cpp').read_text()
logic = source[source.index('static volatile long g_hiResOn'):source.index('static void __fastcall Hook_UpdateViewportRHI')]
config = source[source.index('    g_hiResOn = GetPrivateProfileIntW'):source.index('    g_lookUse = GetPrivateProfileIntW')]
stubs = r'''
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#define sprintf_s(buf, ...) snprintf(buf, sizeof(buf), __VA_ARGS__)
#define _wtof wcstod_value
static double wcstod_value(const wchar_t* s) { return wcstod(s, nullptr); }
static void OLProxyLog(const char*) {}
static std::map<std::wstring, int> ints;
static const wchar_t* scale = L"1.0";
static int GetPrivateProfileIntW(const wchar_t*, const wchar_t* key, int def, const wchar_t*) {
    auto it = ints.find(key); return it == ints.end() ? def : it->second;
}
static void GetPrivateProfileStringW(const wchar_t*, const wchar_t*, const wchar_t*,
                                    wchar_t* out, int size, const wchar_t*) {
    swprintf(out, size, L"%ls", scale);
}
'''
checks = r'''
static void target(long eyeW, long eyeH) {
    long w, h; OLUE3_GetHiResTarget(&w, &h);
    assert(w == eyeW * 2 && h == eyeH);
}
int main() {
    // Missing new key preserves automatic behavior and the old opt-in for HIRES.
    load(); assert(OLUE3_AutoEyeResolution() == 1); assert(g_hiResOn == 0);
    target(1728, 1824);
    ints[L"HighResRender"] = 1; load(); g_hiResHookOk = 1;
    assert(OLUE3_HiResActive() == 1);
    assert(OLUE3_SetHiResEye(2400, 2500) == 1); target(2400, 2496);
    assert(OLUE3_SetHiResEye(2400, 2500) == 0);
    assert(OLUE3_SetRenderScale(0.8f) == 1); target(1920, 2000);
    assert(OLUE3_SetHiResEye(479, 2500) == 0);
    assert(OLUE3_SetHiResEye(2400, 4097) == 0); target(1920, 2000);

    // Manual pixels take precedence over both runtime recommendations and saved scale.
    ints[L"AutoEyeResolution"] = 0;
    ints[L"EyeWidth"] = 1440; ints[L"EyeHeight"] = 1600; scale = L"0.6";
    load(); assert(OLUE3_AutoEyeResolution() == 0); target(1440, 1600);
    assert(OLUE3_SetHiResEye(3000, 3200) == 0); target(1440, 1600);
    assert(OLUE3_SetRenderScale(1.2f) == 0); target(1440, 1600);
    assert(fabsf(OLUE3_GetRenderScale() - 0.6f) < 0.001f);
    g_hiResAppliedW = 2880; g_hiResAppliedH = 1600;
    assert(OLUE3_HiResApplied() == 1);
    ints[L"EyeWidth"] = 1447; ints[L"EyeHeight"] = 1607; load(); target(1440, 1600);
    ints[L"EyeWidth"] = -1; ints[L"EyeHeight"] = 2147483647; load(); target(480, 4096);
    ints.erase(L"EyeWidth"); ints.erase(L"EyeHeight"); load(); target(1728, 1824);

    // Turning HIRES off still bypasses the viewport override in either mode.
    ints[L"HighResRender"] = 0; load(); assert(OLUE3_HiResActive() == 0);
    ints[L"AutoEyeResolution"] = 1; load(); assert(OLUE3_HiResActive() == 0);
    assert(OLUE3_SetHiResEye(2000, 2200) == 1); target(1200, 1320);
    assert(OLUE3_SetRenderScale(0.0f) == 1); target(800, 880);
    assert(OLUE3_SetRenderScale(2.0f) == 1); target(3000, 3296);
    assert(OLUE3_SetRenderScale(std::numeric_limits<float>::quiet_NaN()) == 1);
    target(800, 880);
    puts("Resolution checks passed (AUTO, MANUAL, ini defaults/bounds, scale, runtime updates).");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp) / 'resolution.cpp'
    exe = Path(tmp) / 'resolution_test'
    cpp.write_text(stubs + logic + '\nstatic void load() { const wchar_t* ini = L"test.ini";\n' + config + '}\n' + checks)
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
