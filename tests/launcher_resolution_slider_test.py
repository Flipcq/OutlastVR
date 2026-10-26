"""Run the launcher's actual slider/input/save logic with synthetic INI and sound APIs.
Requires Mono (mcs/mono). No Windows message loop, GPU, or headset is exercised.
Run: python3 tests/launcher_resolution_slider_test.py
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'launcher/Outlast1VR_Pages.cs').read_text()

def between(start, end):
    return source[source.index(start):source.index(end)]

definitions = between('    enum SetKind', '    // outlastvr.ini')
model = between('    sealed class EyeResolutionRange', '    sealed partial class Viewfinder')
defs = between('        static readonly SettingDef[] Defs', '        double[] setVals')
loading = between('        double[] setVals', '        // --settings-test')
values = between('        int ChoiceIndex', '        // ---------------- manual resolution slider')
slider = between('        // ---------------- manual resolution slider', '        void RunSettingAction')
lines = between('        // The list as drawn:', '        // ---------------- the controls tape')
stubs = r'''
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Drawing;
using System.IO;
enum Tone { Err, Warn, Info, Ok }
static class SysInfo { public static bool GameRunning() { return false; } }
sealed class Sound { public void Error() {} public void Confirm() {} public void Tick() {} }
static class ModIni {
    public static Dictionary<string, string> Data = new Dictionary<string, string>();
    public static string FailKey;
    public static int Writes;
    public static string Read(string file, string section, string key, string def) {
        string value; return Data.TryGetValue(key, out value) ? value : def;
    }
    public static bool Write(string file, string section, string key, string value) {
        Writes++;
        if (key == FailKey) return false;
        if (value == null) Data.Remove(key); else Data[key] = value;
        return true;
    }
}
'''
fixture = r'''
sealed partial class Viewfinder {
    string gameDir;
    double T;
    bool preview, settingsWriteTest;
    Sound sfx = new Sound();
    const int PageSettings = 1;
    int page = PageSettings;
    void RunSettingAction(SettingDef d) {}
'''
checks = r'''
    static void Check(bool value, string message) {
        if (!value) throw new Exception(message);
    }
    int Find(string key) { return Array.FindIndex(Defs, d => d.Key == key); }
    void Ready() {
        ModIni.Data.Clear(); ModIni.Writes = 0; ModIni.FailKey = null;
        ModIni.Data["HighResRender"] = "1"; ModIni.Data["AutoEyeResolution"] = "0";
        ModIni.Data["EyeWidth"] = "2496"; ModIni.Data["EyeHeight"] = "2688";
        LoadSettings(); SettingsSelect(Find("EyeWidth"));
    }
    static void RangeChecks() {
        foreach (int[] size in new[] { new[] {2496, 2688}, new[] {1728, 1824},
                 new[] {1600, 1440}, new[] {480, 4096}, new[] {4096, 480}, new[] {1024, 1024} }) {
            var range = new EyeResolutionRange(size[0], size[1]);
            int lastW = 0, lastH = 0;
            for (int i = 0; i <= 1000; i++) {
                int w, h; range.SizeAt(i / 1000.0, out w, out h);
                Check(w >= 480 && h >= 480 && w <= 4096 && h <= 4096, "bounds");
                Check(w % 8 == 0 && h % 8 == 0, "8-pixel alignment");
                Check(w >= lastW && h >= lastH, "monotonic slider");
                Check(Math.Abs(w * (double)size[1] - h * (double)size[0]) / Math.Max(size[0], size[1]) <= 4.01, "aspect ratio rounding");
                lastW = w; lastH = h;
            }
            int rw, rh; range.SizeAt(range.Position(size[0], size[1]), out rw, out rh);
            Check(rw == size[0] && rh == size[1], "saved resolution round trip");
        }
    }
    public static void Main(string[] args) {
        RangeChecks();
        var v = new Viewfinder { gameDir = args[0] };
        File.WriteAllText(Path.Combine(args[0], "outlastvr.ini"), "synthetic fixture");
        v.Ready();
        Check(v.ValueText(v.Find("EyeWidth")) == "2496 x 2688", "paired label");
        var track = ResolutionTrack(v.ResolutionRow());
        var left = new Point(track.Left, track.Top + track.Height / 2);
        var right = new Point(track.Right, left.Y);
        Check(v.BeginResolutionDrag(left), "start drag");
        Check(ModIni.Writes == 0, "no writes on mouse down");
        v.SettingsSelect(0); v.Wheel(-120);
        Check(v.setSel == v.Find("EyeWidth"), "selection stays fixed during drag");
        Check(v.DragResolution(right), "drag right");
        Check(v.manualEyeH == 4096 && ModIni.Writes == 0, "live display without persistence");
        Check(v.EndResolutionDrag(new Point(right.X + 1000, right.Y)), "release outside track");
        Check(ModIni.Writes == 2 && ModIni.Data["EyeHeight"] == "4096", "save pair once on release");
        Check(!v.EndResolutionDrag(right), "no duplicate save");
        v.Ready(); v.BeginResolutionDrag(right); v.CancelResolutionDrag();
        Check(v.manualEyeW == 2496 && v.manualEyeH == 2688 && ModIni.Writes == 0, "lost capture cancels");
        v.Ready(); v.ChangeSetting(v.Find("EyeWidth"), -1);
        Check(v.manualEyeH == 2680 && ModIni.Writes == 2, "keyboard/controller step saves");
        Check(!ModIni.Data["EyeWidth"].Contains("."), "integer ini serialization");
        int w = v.manualEyeW, h = v.manualEyeH;
        v.LoadSettings(); Check(v.manualEyeW == w && v.manualEyeH == h, "reload saved pair");
        v.Ready(); v.setVals[v.Find("AutoEyeResolution")] = 1;
        Check(!v.BeginResolutionDrag(left), "AUTO disables drag");
        v.ChangeSetting(v.Find("EyeWidth"), 1); Check(ModIni.Writes == 0, "AUTO disables stepping");
        v.Ready(); v.setVals[v.Find("HighResRender")] = 0;
        Check(!v.BeginResolutionDrag(left), "HIRES off disables slider");
        v.Ready(); v.iniOk = false; Check(!v.BeginResolutionDrag(left), "missing ini disables slider");
        v.Ready(); ModIni.FailKey = "EyeHeight";
        v.BeginResolutionDrag(right); v.EndResolutionDrag(right);
        Check(ModIni.Data["EyeWidth"] == "2496" && ModIni.Data["EyeHeight"] == "2688", "rollback on second write failure");
        Check(v.manualEyeW == 2496 && v.manualEyeH == 2688 && v.setStatusKind == Tone.Err, "failure reloads actual file");
        v.Ready(); ModIni.FailKey = "EyeWidth"; v.ChangeSetting(v.Find("EyeWidth"), 1);
        Check(ModIni.Writes == 1 && v.manualEyeW == 2496, "first write failure");
        v.Ready(); v.preview = true; v.ChangeSetting(v.Find("EyeWidth"), 1);
        Check(ModIni.Writes == 0 && v.manualEyeH > 2688, "preview never writes");
        v.preview = false; v.Ready(); ModIni.Data.Remove("EyeWidth"); ModIni.Data["EyeHeight"] = "NaN";
        v.LoadSettings(); Check(v.manualEyeW == 1728 && v.manualEyeH == 1824, "missing/invalid defaults");
        ModIni.Data["EyeWidth"] = "2147483647"; ModIni.Data["EyeHeight"] = "-1";
        v.LoadSettings(); Check(v.manualEyeW == 4096 && v.manualEyeH == 480, "invalid bounds");
        Console.WriteLine("Slider checks passed: range/aspect, mouse drag/release/cancel, arrows, gating, persistence and failures.");
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cs = Path(tmp) / 'slider_test.cs'
    exe = Path(tmp) / 'slider_test.exe'
    cs.write_text(stubs + definitions + model + fixture + defs + loading + values + slider + lines + checks)
    subprocess.run(['mcs', '-langversion:5', '-r:System.Drawing.dll', '-out:' + str(exe), str(cs)], check=True)
    subprocess.run(['mono', str(exe), tmp], check=True)
