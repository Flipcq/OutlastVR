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

        // (Beta 1.1.4) typed resolution: the numbers beside the slider take a keyboard size.
        v.Ready();
        Check(v.BeginResolutionTyping() && v.ResolutionTyping, "clicking the numbers starts typing");
        foreach (char c in "2560x1440") v.ResolutionTypeChar(c);
        Check(v.ResolutionTyped == "2560x1440", "digits and the separator are kept");
        Check(v.ApplyResolutionTyped() && !v.ResolutionTyping, "ENTER applies and closes the field");
        Check(ModIni.Writes == 2 && ModIni.Data["EyeWidth"] == "2560" && ModIni.Data["EyeHeight"] == "1440", "typed pair saved");
        Check(v.manualEyeW == 2560 && v.manualEyeH == 1440 && v.setStatusKind == Tone.Ok, "typed pair applied");
        // one number = the pair's larger side, aspect kept (2496x2688 -> 3000 -> 2784x3000), like the slider
        v.Ready(); v.BeginResolutionTyping();
        foreach (char c in "3000") v.ResolutionTypeChar(c);
        Check(v.ApplyResolutionTyped() && v.manualEyeH == 3000 && v.manualEyeW == 2784, "one number keeps the aspect");
        // the same Normalize the slider uses: 480 - 4096 per eye, 8-pixel steps
        v.Ready(); v.BeginResolutionTyping();
        foreach (char c in "9999x100") v.ResolutionTypeChar(c);
        Check(v.ApplyResolutionTyped() && v.manualEyeW == 4096 && v.manualEyeH == 480, "typing respects the bounds");
        v.Ready(); v.BeginResolutionTyping();
        foreach (char c in "2561x1439") v.ResolutionTypeChar(c);
        Check(v.ApplyResolutionTyped() && v.manualEyeW == 2560 && v.manualEyeH == 1432, "typing snaps to 8 pixels");
        // a typo keeps the field open and writes nothing
        v.Ready(); v.BeginResolutionTyping(); v.ResolutionTypeChar('7'); v.ResolutionTypeChar('x');
        Check(!v.ApplyResolutionTyped() && v.ResolutionTyping && ModIni.Writes == 0, "half a size is not saved");
        Check(v.setStatusKind == Tone.Err, "and the tape log says why");
        v.ResolutionBackspace();
        Check(v.ResolutionTyped == "7", "backspace drops a separator whole");
        v.CancelResolutionTyping();
        Check(!v.ResolutionTyping && ModIni.Writes == 0, "ESC cancels without writing");
        // the same size again is not a change
        v.Ready(); v.BeginResolutionTyping();
        foreach (char c in "2496x2688") v.ResolutionTypeChar(c);
        Check(v.ApplyResolutionTyped() && ModIni.Writes == 0, "an unchanged size writes nothing");
        // separators: space, comma and the letter X all work
        v.Ready(); v.BeginResolutionTyping();
        foreach (char c in "1920 1080") v.ResolutionTypeChar(c);
        Check(v.ResolutionTyped == "1920x1080" && v.ApplyResolutionTyped() && v.manualEyeW == 1920, "spaces become a separator");
        // gating: the same two switches the slider itself obeys
        v.Ready(); v.setVals[v.Find("AutoEyeResolution")] = 1;
        Check(!v.BeginResolutionTyping(), "AUTO disables typing");
        v.Ready(); v.iniOk = false; Check(!v.BeginResolutionTyping(), "a missing ini disables typing");

        // (Beta 1.1.4) the stick dead zone row: 0.30 by default and a hard 0.60 ceiling in the launcher.
        v.Ready(); int dz = v.Find("StickDeadzone");
        Check(dz >= 0, "the launcher has a stick dead zone row");
        Check(Math.Abs(Defs[dz].Def - 0.30) < 1e-9 && Math.Abs(Defs[dz].Max - 0.60) < 1e-9, "0.30 default, 0.60 maximum");
        Check(v.ValueText(dz) == "30%", "the row reads 30% out of the box");
        ModIni.Data["StickDeadzone"] = "0.95"; v.LoadSettings();
        Check(Math.Abs(v.setVals[dz] - 0.60) < 1e-9, "a dead zone above the ceiling is clamped on load");
        Check(v.ValueText(dz) == "60%", "and shown at the ceiling");
        v.ChangeSetting(dz, 1);
        Check(Math.Abs(v.setVals[dz] - 0.60) < 1e-9 && ModIni.Data["StickDeadzone"] == "0.600", "and cannot be raised past it");
        ModIni.Data["StickDeadzone"] = "0.00"; v.LoadSettings();
        Check(v.ValueText(dz) == "GAME'S", "0 is the game's own dead zone");
        Console.WriteLine("Slider checks passed: range/aspect, mouse drag/release/cancel, arrows, gating, persistence and failures.");
        Console.WriteLine("Typed resolution: click, digits/separators, apply/cancel/backspace, bounds, aspect and writes.");
        Console.WriteLine("Stick dead zone: 0.30 default, 0.60 ceiling, clamped on load and on change.");
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cs = Path(tmp) / 'slider_test.cs'
    exe = Path(tmp) / 'slider_test.exe'
    cs.write_text(stubs + definitions + model + fixture + defs + loading + values + slider + lines + checks)
    subprocess.run(['mcs', '-langversion:5', '-r:System.Drawing.dll', '-out:' + str(exe), str(cs)], check=True)
    subprocess.run(['mono', str(exe), tmp], check=True)
