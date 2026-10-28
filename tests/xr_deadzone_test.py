"""Check the stick / trigger dead-zone shaping the XR input path hands to the game.

Runs the real ApplyDeadZone and TriggerUnit from src/ol_xr.cpp against synthetic values (g++).
Does not replace a live controller test: it proves the arithmetic, not the hardware.
Run: python3 tests/xr_deadzone_test.py
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/ol_xr.cpp').read_text()
# The pure shaping only: the trigger threshold, the stick shaping and the trigger curve, without the
# XR-facing state around them.
kz = source.index('static const float    kTriggerDeadzone')
kz_line = source[kz:source.index('\n', kz)]
logic = kz_line + '\n' + source[source.index('static void ApplyDeadZone'):
                               source.index('static void StickCenter(int hand')]
ini = (ROOT / 'runtime/outlastvr.ini').read_text()
launcher = (ROOT / 'launcher/Outlast1VR_Pages.cs').read_text()

stubs = r'''
#include <cassert>
#include <cmath>
#include <cstdio>
'''

checks = r'''
static void dz(float d, float inX, float inY, float* outX, float* outY) {
    *outX = inX; *outY = inY;
    ApplyDeadZone(d, outX, outY);
}
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
static float mag(float x, float y) { return std::sqrt(x * x + y * y); }
int main() {
    float x, y;
    // 0.00 means "the game's own dead zone": the raw value passes through untouched.
    dz(0.0f, 0.11f, -0.07f, &x, &y); assert(near(x, 0.11f) && near(y, -0.07f));

    // A resting stick under the dead zone gives nothing at all.
    dz(0.30f, 0.00f, 0.00f, &x, &y); assert(x == 0.0f && y == 0.0f);
    dz(0.30f, 0.29f, 0.00f, &x, &y); assert(x == 0.0f && y == 0.0f);
    dz(0.30f, -0.20f, 0.10f, &x, &y); assert(x == 0.0f && y == 0.0f);

    // The veer: a stick pushed forward that also reads a little to the left must walk STRAIGHT.
    // A radial dead zone alone kept the sideways part of this - it is past the dead zone overall -
    // which is what "Miles runs but drifts left, even with W" was.
    dz(0.30f, 0.05f, 0.95f, &x, &y);
    assert(x == 0.0f && near(y, (0.95f - 0.30f) / 0.70f));
    dz(0.30f, -0.29f, 1.00f, &x, &y); assert(x == 0.0f && near(y, 1.0f));
    dz(0.30f, 0.0f, -0.60f, &x, &y); assert(x == 0.0f && y < -0.4f);

    // A real sideways push still works, and full stick is still full stick.
    dz(0.30f, 0.50f, 0.0f, &x, &y); assert(y == 0.0f && near(x, (0.50f - 0.30f) / 0.70f));
    dz(0.30f, 1.0f, 0.0f, &x, &y); assert(near(x, 1.0f) && y == 0.0f);
    dz(0.30f, -0.65f, 0.0f, &x, &y); assert(x < 0.0f && near(-x, (0.65f - 0.30f) / 0.70f));

    // Diagonals keep their direction and never come out longer than the stick really is.
    dz(0.30f, 0.60f, 0.60f, &x, &y);
    assert(near(x, y) && mag(x, y) > 0.7f && mag(x, y) < 0.8f);
    dz(0.30f, 1.0f, 1.0f, &x, &y);
    assert(near(x, 0.7071f) && near(y, 0.7071f) && mag(x, y) <= 1.0f);

    // A dead zone of 1 (never written, but a corrupt ini could) must not divide by zero or flip a sign.
    dz(1.0f, 0.3f, 0.4f, &x, &y); assert(x == 0.0f && y == 0.0f);

    // Triggers are Outlast's analog lean: a resting trigger is not a pull, a full pull is still full.
    assert(TriggerUnit(0.0f) == 0.0f);
    assert(TriggerUnit(0.05f) == 0.0f);
    assert(TriggerUnit(kTriggerDeadzone) == 0.0f);
    assert(TriggerUnit(std::nanf("")) == 0.0f);
    assert(near(TriggerUnit(0.56f), (0.56f - 0.12f) / 0.88f));
    assert(near(TriggerUnit(1.0f), 1.0f));
    assert(near(TriggerUnit(2.0f), 1.0f));
    std::printf("Dead zone: per-axis cut, radial curve, clamp and trigger threshold passed.\n");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'dz.cpp'
    exe = Path(tmp) / 'dz-test'
    src.write_text(stubs + logic + checks)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# The shipped default and the launcher's ceiling, which is what a player actually gets.
assert 'StickDeadzone=0.30' in ini, 'the shipped ini must default the dead zone to 0.30'
assert 'L"0.30"' in source and 'StickDeadzone' in source, 'the code default must be 0.30 too'
assert 'd > 0.60f' in source, 'the mod clamps the dead zone at 0.60'
block = launcher[launcher.index('"StickDeadzone"'):]
block = block[:block.index('\n')]
assert '0.30' in block and '0.60' in block, 'the launcher offers 0.30 with a 0.60 ceiling'
print('XR dead zone: default 0.30, 0.60 ceiling, straight-line walk and trigger threshold checks passed.')
