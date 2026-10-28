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

        // (Beta 1.1.4) typed resolution: the width and the height are two separate fields.
        v.Ready();
        Check(v.BeginResolutionEdit(0) && v.ResolutionEditing && v.ResolutionEditField == 0, "clicking the width opens the width field");
        foreach (char c in "2560") v.ResolutionEditChar(c);
        Check(v.ResolutionEdited == "2560", "digits are kept");
        Check(v.ApplyResolutionEdit() && !v.ResolutionEditing, "ENTER applies and closes the field");
        Check(ModIni.Writes == 2 && ModIni.Data["EyeWidth"] == "2560" && ModIni.Data["EyeHeight"] == "2688", "only the width was written");
        Check(v.manualEyeW == 2560 && v.manualEyeH == 2688 && v.setStatusKind == Tone.Ok, "the width was applied");
        // ...and then the height is its own field
        Check(v.BeginResolutionEdit(1) && v.ResolutionEditField == 1, "the height opens on its own");
        foreach (char c in "1440") v.ResolutionEditChar(c);
        Check(v.ApplyResolutionEdit() && v.manualEyeW == 2560 && v.manualEyeH == 1440, "then the height");
        Check(ModIni.Data["EyeWidth"] == "2560" && ModIni.Data["EyeHeight"] == "1440", "both halves saved");
        // clicking the other number while one is open commits it first, which is the "click, change, click the
        // other, change" flow: no ENTER needed between the two
        v.Ready(); v.BeginResolutionEdit(0);
        foreach (char c in "1536") v.ResolutionEditChar(c);
        Check(v.BeginResolutionEdit(1) && v.manualEyeW == 1536 && v.ResolutionEditField == 1, "clicking the height commits the width first");
        Check(v.ResolutionEdited.Length == 0, "the new field starts empty");
        Check(ModIni.Data["EyeWidth"] == "1536" && ModIni.Data["EyeHeight"] == "2688", "the first field reached the ini");
        v.CancelResolutionEdit();
        // a field holds one number, so the bounds are the slider's Normalize: 480 - 4096, 8-pixel steps
        v.Ready(); v.BeginResolutionEdit(0);
        foreach (char c in "9999") v.ResolutionEditChar(c);
        Check(v.ApplyResolutionEdit() && v.manualEyeW == 4096, "a too-large number lands on the ceiling");
        v.Ready(); v.BeginResolutionEdit(1);
        foreach (char c in "100") v.ResolutionEditChar(c);
        Check(v.ApplyResolutionEdit() && v.manualEyeH == 480, "a too-small number lands on the floor");
        v.Ready(); v.BeginResolutionEdit(0);
        foreach (char c in "2561") v.ResolutionEditChar(c);
        Check(v.ApplyResolutionEdit() && v.manualEyeW == 2560, "typing snaps to 8 pixels");
        // nothing typed is refused, and the field stays open so ENTER again cannot silently change anything
        v.Ready(); v.BeginResolutionEdit(0);
        Check(!v.ApplyResolutionEdit() && v.ResolutionEditing && ModIni.Writes == 0, "an empty field is refused");
        Check(v.setStatusKind == Tone.Err, "and the tape log says why");
        v.ResolutionEditChar('7'); v.ResolutionEditChar('0'); v.ResolutionBackspace();
        Check(v.ResolutionEdited == "7", "backspace edits the number");
        v.CancelResolutionEdit();
        Check(!v.ResolutionEditing && ModIni.Writes == 0, "ESC cancels without writing");
        // the same number again is not a change
        v.Ready(); v.BeginResolutionEdit(0);
        foreach (char c in "2496") v.ResolutionEditChar(c);
        Check(v.ApplyResolutionEdit() && ModIni.Writes == 0, "an unchanged width writes nothing");
        // clicking the same field twice keeps what was typed, and switching with nothing typed is free
        v.Ready(); v.BeginResolutionEdit(0); v.ResolutionEditChar('1'); v.ResolutionEditChar('2');
        Check(v.BeginResolutionEdit(0) && v.ResolutionEdited == "12", "re-clicking the open field keeps the draft");
        v.CancelResolutionEdit();
        v.Ready(); Check(v.BeginResolutionEdit(0) && v.BeginResolutionEdit(1) && v.ResolutionEditField == 1, "an empty field does not block the other one");
        // leaving the field saves it; an empty one just closes
        v.Ready(); v.BeginResolutionEdit(0);
        foreach (char c in "1600") v.ResolutionEditChar(c);
        v.LeaveResolutionEdit();
        Check(!v.ResolutionEditing && v.manualEyeW == 1600 && ModIni.Data["EyeWidth"] == "1600", "leaving the field saves it");
        v.Ready(); v.BeginResolutionEdit(1); v.LeaveResolutionEdit();
        Check(!v.ResolutionEditing && ModIni.Writes == 0, "an empty field closes without writing");
        // gating: the same two switches the slider itself obeys
        v.Ready(); v.setVals[v.Find("AutoEyeResolution")] = 1;
        Check(!v.BeginResolutionEdit(0), "AUTO disables typing");
        v.Ready(); v.iniOk = false; Check(!v.BeginResolutionEdit(1), "a missing ini disables typing");

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
