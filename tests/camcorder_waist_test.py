"""Run the actual waist grab-zone geometry used for taking the camcorder from the hip.
python3 tests/camcorder_waist_test.py (g++). Covers the seated lap reach too.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
interact = (ROOT / 'src/ol_interact.cpp').read_text()
start = interact.index('static const float kWaistSideM')
end = interact.index('enum { H_HOLSTERED', start)
zone = interact[start:end]

STUBS = r'''
#include <cassert>
#include <cstdio>
struct V3 { float x, y, z; };
static V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 s_torsoF = { 0.0f, 0.0f, -1.0f };            // facing forward (-z)
static V3 TorsoRight() { return { -s_torsoF.z, 0.0f, s_torsoF.x }; }
'''

CASES = r'''
int main() {
    const V3 hip = { 0, 0, 0 };
    assert(InWaistZone({ 0, 0, 0 }, hip));                 // on the holster
    assert(InWaistZone({ 0.18f, -0.05f, -0.10f }, hip));   // a hand at the right hip
    assert(InWaistZone({ 0.00f, -0.10f, -0.30f }, hip));   // seated: hand in the lap, forward and down
    assert(InWaistZone({ 0.00f, -0.25f, -0.20f }, hip));   // seated: lower than the hip, still forward
    assert(InWaistZone({ 0.00f, 0.20f, 0.00f }, hip));     // a hand coming down from above
    assert(!InWaistZone({ 0.40f, 0.00f, 0.00f }, hip));    // far out to the side
    assert(!InWaistZone({ 0.00f, 0.00f, -0.50f }, hip));   // far in front
    assert(!InWaistZone({ 0.00f, -0.40f, 0.00f }, hip));   // below the waist zone
    puts("Camcorder waist zone: hip, standing reach and seated lap pass; far points stay out.");
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / 'test.cpp'
        src.write_text(STUBS + zone + CASES)
        exe = Path(d) / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
