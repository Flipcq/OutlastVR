// ctrlcard: the tester control cards for Outlast VR, in the launcher's found-footage camcorder look.
// Words only (no controller pictures). Usage: ctrlcard.exe <outDir>
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;
using System.Runtime.InteropServices;

static class Card
{
    const int W = 1920, H = 1080;
    static readonly Color Ink = Color.FromArgb(222, 244, 222);       // bright text
    static readonly Color Soft = Color.FromArgb(156, 204, 160);      // body text
    static readonly Color Head = Color.FromArgb(150, 255, 160);      // section headers
    static readonly Color Line = Color.FromArgb(90, 150, 100);       // rules, tag borders
    static readonly Color Hud = Color.FromArgb(236, 240, 236);
    static readonly Color Rec = Color.FromArgb(232, 28, 28);
    static string Fam = "Courier New";

    static Font F(float px, bool bold) { return new Font(Fam, px, bold ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel); }
    static readonly StringFormat SF = MakeSF();
    static StringFormat MakeSF() { StringFormat s = (StringFormat)StringFormat.GenericTypographic.Clone(); s.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces; return s; }
    static float Wd(Graphics g, string s, Font f) { return g.MeasureString(s, f, PointF.Empty, SF).Width; }

    static void Txt(Graphics g, string s, Font f, Color c, float x, float y, float glow)
    {
        if (glow > 0)
        {
            using (SolidBrush gb = new SolidBrush(Color.FromArgb((int)(38 * glow), c.R / 2, 255, c.B / 2)))
                for (int dx = -2; dx <= 2; dx += 2) for (int dy = -2; dy <= 2; dy += 2) if (dx != 0 || dy != 0) g.DrawString(s, f, gb, x + dx, y + dy, SF);
        }
        using (SolidBrush sh = new SolidBrush(Color.FromArgb(150, 0, 0, 0))) g.DrawString(s, f, sh, x + 2, y + 2, SF);
        using (SolidBrush b = new SolidBrush(c)) g.DrawString(s, f, b, x, y, SF);
    }

    // the title: a little colour fringing, like a cheap camcorder lens
    static void Title(Graphics g, string s, Font f, float x, float y)
    {
        using (SolidBrush r = new SolidBrush(Color.FromArgb(110, 255, 40, 40))) g.DrawString(s, f, r, x - 3, y, SF);
        using (SolidBrush c = new SolidBrush(Color.FromArgb(110, 40, 230, 255))) g.DrawString(s, f, c, x + 3, y + 1, SF);
        Txt(g, s, f, Ink, x, y, 1.6f);
    }

    static List<string> Wrap(Graphics g, string s, Font f, float width)
    {
        List<string> lines = new List<string>(); string cur = "";
        foreach (string w in s.Split(' '))
        {
            string t = cur.Length == 0 ? w : cur + " " + w;
            if (Wd(g, t, f) > width && cur.Length > 0) { lines.Add(cur); cur = w; } else cur = t;
        }
        if (cur.Length > 0) lines.Add(cur);
        return lines;
    }

    // a button name in a box, then what it does; returns the row's height
    static float Row(Graphics g, string tag, string text, float x, float y, float tagW, float colW, Font tf, Font bf)
    {
        float tw = Wd(g, tag, tf), th = tf.Size + 12;
        using (Pen p = new Pen(Line, 2f)) g.DrawRectangle(p, x, y - 4, tagW, th);
        using (SolidBrush fill = new SolidBrush(Color.FromArgb(70, 20, 60, 28))) g.FillRectangle(fill, x + 1, y - 3, tagW - 1, th - 1);
        Txt(g, tag, tf, Ink, x + (tagW - tw) / 2, y + 2, 0.6f);
        List<string> lines = Wrap(g, text, bf, colW - tagW - 24);
        float ly = y + 1;
        foreach (string l in lines) { Txt(g, l, bf, Soft, x + tagW + 22, ly, 0); ly += bf.Size + 8; }
        return Math.Max(th + 10, lines.Count * (bf.Size + 8) + 10);
    }

    static void Header(Graphics g, string s, float x, float y, float w)
    {
        Font f = F(30, true);
        Txt(g, s, f, Head, x, y, 1.0f);
        using (Pen p = new Pen(Line, 2f)) g.DrawLine(p, x, y + 42, x + w, y + 42);
    }

    static void Bracket(Graphics g, Pen p, float x, float y, float dx, float dy) { g.DrawLine(p, x, y, x + dx, y); g.DrawLine(p, x, y, x, y + dy); }

    static void HudFrame(Graphics g, string tape, string time)
    {
        using (Pen p = new Pen(Color.FromArgb(225, Hud), 4f))
        {
            const float m = 40, L = 70;
            Bracket(g, p, m, m, L, L); Bracket(g, p, W - m, m, -L, L); Bracket(g, p, m, H - m, L, -L); Bracket(g, p, W - m, H - m, -L, -L);
        }
        using (SolidBrush r = new SolidBrush(Rec)) g.FillEllipse(r, 84, 66, 26, 26);
        using (SolidBrush r = new SolidBrush(Color.FromArgb(70, Rec))) g.FillEllipse(r, 78, 60, 38, 38);
        Font hf = F(34, true), sf = F(20, true);
        Txt(g, "REC", hf, Hud, 124, 62, 0);
        Txt(g, time, hf, Hud, 240, 62, 0);
        Txt(g, "SP   " + tape, sf, Hud, 126, 106, 0);
        // top right: night vision and the battery
        Font nf = F(24, true);
        float bx = W - 168, by = 66;
        using (Pen p = new Pen(Hud, 3f)) g.DrawRectangle(p, bx, by, 64, 28);
        using (SolidBrush b = new SolidBrush(Hud)) { g.FillRectangle(b, bx + 64, by + 8, 6, 12); for (int i = 0; i < 3; ++i) g.FillRectangle(b, bx + 6 + i * 19, by + 6, 14, 16); }
        string nv = "NIGHT VISION"; float nw = Wd(g, nv, nf);
        Txt(g, nv, nf, Hud, bx - 26 - nw, by + 1, 0);
        using (SolidBrush b = new SolidBrush(Hud)) { g.FillEllipse(b, bx - 26 - nw - 34, by + 2, 24, 24); }
        using (SolidBrush b = new SolidBrush(Color.FromArgb(18, 34, 22))) g.FillEllipse(b, bx - 26 - nw - 27, by - 3, 24, 24);
        // the zoom scale on the right
        using (Pen p = new Pen(Color.FromArgb(200, Hud), 3f))
        {
            float zx = W - 76, z0 = 300, z1 = 760;
            g.DrawLine(p, zx, z0, zx, z1);
            for (int i = 0; i <= 10; ++i) g.DrawLine(p, zx - (i % 5 == 0 ? 14 : 8), z0 + (z1 - z0) * i / 10f, zx, z0 + (z1 - z0) * i / 10f);
        }
        Font zf = F(20, true);
        Txt(g, "T", zf, Hud, W - 82, 268, 0); Txt(g, "W", zf, Hud, W - 84, 770, 0);
        using (SolidBrush b = new SolidBrush(Hud)) g.FillPolygon(b, new[] { new PointF(W - 92, 690), new PointF(W - 104, 682), new PointF(W - 104, 698) });
        // the date stamp
        Font df = F(30, true);
        string d1 = "OCT 31 2013", d2 = "11:57:16 PM";
        Txt(g, d1, df, Hud, W - 110 - Wd(g, d1, df), H - 132, 0);
        Txt(g, d2, F(38, true), Hud, W - 110 - Wd(g, d2, F(38, true)), H - 94, 0);
    }

    static void Background(Graphics g, int seed)
    {
        using (LinearGradientBrush lg = new LinearGradientBrush(new Rectangle(0, 0, W, H), Color.FromArgb(10, 24, 13), Color.FromArgb(4, 11, 6), 90f)) g.FillRectangle(lg, 0, 0, W, H);
        using (GraphicsPath gp = new GraphicsPath())
        {
            gp.AddEllipse(-W * 0.15f, -H * 0.35f, W * 1.3f, H * 1.7f);
            using (PathGradientBrush pg = new PathGradientBrush(gp))
            {
                pg.CenterColor = Color.FromArgb(150, 36, 82, 44);
                pg.SurroundColors = new[] { Color.FromArgb(0, 0, 0, 0) };
                pg.CenterPoint = new PointF(W * 0.62f, H * 0.45f);
                g.FillRectangle(pg, 0, 0, W, H);
            }
        }
        // faint corridor: a lit doorway far away, the floor and ceiling lines running to it
        using (Pen p = new Pen(Color.FromArgb(20, 140, 220, 150), 3f))
        {
            PointF v = new PointF(W * 0.66f, H * 0.47f);
            foreach (PointF e in new[] { new PointF(W * 0.36f, H), new PointF(W * 1.0f, H), new PointF(W * 0.36f, 0), new PointF(W * 1.0f, 0), new PointF(W * 0.5f, H), new PointF(W * 0.84f, H) })
                g.DrawLine(p, e, v);
        }
        using (SolidBrush b = new SolidBrush(Color.FromArgb(16, 170, 255, 180))) g.FillRectangle(b, W * 0.66f - 34, H * 0.47f - 70, 68, 120);
        using (SolidBrush b = new SolidBrush(Color.FromArgb(24, 210, 255, 210))) g.FillRectangle(b, W * 0.66f - 16, H * 0.47f - 46, 32, 80);
    }

    // grain, scanlines, a vignette, two torn lines - the tape look, over everything
    static void Tape(Bitmap bmp, int seed, int[] tear)
    {
        Rectangle r = new Rectangle(0, 0, W, H);
        BitmapData d = bmp.LockBits(r, ImageLockMode.ReadWrite, PixelFormat.Format32bppArgb);
        int stride = d.Stride; byte[] px = new byte[stride * H];
        Marshal.Copy(d.Scan0, px, 0, px.Length);
        Random rnd = new Random(seed);
        for (int y = 0; y < H; ++y)
        {
            int shift = 0;
            foreach (int t in tear) if (y >= t && y < t + 5) shift = 14;
            float scan = (y % 3 == 0) ? 0.80f : 1.0f;
            float vy = (y - H * 0.5f) / (H * 0.5f);
            for (int x = 0; x < W; ++x)
            {
                int sx = x - shift; if (sx < 0) sx = 0;
                int i = y * stride + x * 4, s = y * stride + sx * 4;
                float vx = (x - W * 0.5f) / (W * 0.5f);
                float vig = 1.0f - 0.42f * (vx * vx * 0.55f + vy * vy * 0.85f);
                int n = rnd.Next(-16, 17);
                float boost = shift != 0 ? 1.25f : 1.0f;
                for (int c = 0; c < 3; ++c)
                {
                    float v = (px[s + c] * scan * vig * boost) + n * (c == 1 ? 1.0f : 0.75f);
                    px[i + c] = (byte)(v < 0 ? 0 : (v > 255 ? 255 : v));
                }
            }
        }
        Marshal.Copy(px, 0, d.Scan0, px.Length);
        bmp.UnlockBits(d);
    }

    static Graphics Begin(Bitmap b)
    {
        Graphics g = Graphics.FromImage(b);
        g.SmoothingMode = SmoothingMode.AntiAlias; g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
        g.CompositingQuality = CompositingQuality.HighQuality;
        return g;
    }

    static void Footer(Graphics g)
    {
        Txt(g, "OUTLAST VR  BETA 1.0  -  VR mod made by StrikeZoneVR", F(22, true), Soft, 112, H - 86, 0);
    }

    static void Controls(string path)
    {
        using (Bitmap b = new Bitmap(W, H, PixelFormat.Format32bppArgb))
        {
            using (Graphics g = Begin(b))
            {
                Background(g, 3);
                HudFrame(g, "TAPE 03", "00:00:03:15");
                Title(g, "QUEST 3 CONTROLS", F(80, true), 112, 140);
                Txt(g, "TAPE 03 - WHAT EVERY BUTTON DOES", F(24, true), Soft, 116, 236, 0);
                Font tf = F(21, true), bf = F(24, true);
                float lx = 112, rx = 990, cw = 820, tagW = 196, y0 = 300;
                Header(g, "LEFT CONTROLLER", lx, y0, cw); Header(g, "RIGHT CONTROLLER", rx, y0, cw);
                string[,] L = {
                    { "STICK", "Walk" }, { "STICK CLICK", "Sprint" }, { "GRIP", "Sprint (hold). On a ledge, rung or door handle: hold on" },
                    { "X", "Use, open, pick up" }, { "Y", "Reload the camcorder" }, { "TRIGGER", "Lean left, look back" }, { "MENU", "Pause" } };
                string[,] R = {
                    { "STICK", "Turn (smooth or snap)" }, { "STICK CLICK", "Night vision" }, { "STICK UP/DOWN", "Zoom, camcorder at your eye" },
                    { "GRIP AT HIP", "Take the camcorder" }, { "A", "Jump, climb, vault" }, { "B", "Crouch. On a ladder: let go" },
                    { "TRIGGER", "Lean right, look back" }, { "META", "The headset's own menu" } };
                float y = y0 + 64;
                for (int i = 0; i < L.GetLength(0); ++i) y += Row(g, L[i, 0], L[i, 1], lx, y, tagW, cw, tf, bf);
                float y2 = y0 + 64;
                for (int i = 0; i < R.GetLength(0); ++i) y2 += Row(g, R[i, 0], R[i, 1], rx, y2, tagW, cw, tf, bf);
                float yb = Math.Max(y, y2) + 4;
                Header(g, "MENUS AND COMBOS", lx, yb, cw * 2 + (rx - lx - cw));
                string[,] C = {
                    { "L GRIP + X", "Notes" }, { "L GRIP + Y", "Documents" }, { "L GRIP + B", "Reporter's notebook" },
                    { "L GRIP + MENU", "VR settings" }, { "BOTH CLICKS", "Recenter (both sticks)" }, { "IN MENUS", "Left stick moves, A picks, B back" } };
                float cy = yb + 64, cy2 = yb + 64;
                for (int i = 0; i < 3; ++i) cy += Row(g, C[i, 0], C[i, 1], lx, cy, tagW, cw, tf, bf);
                for (int i = 3; i < 6; ++i) cy2 += Row(g, C[i, 0], C[i, 1], rx, cy2, tagW, cw, tf, bf);
                Footer(g);
                Console.WriteLine("controls: columns end at {0:0} / {1:0}, combos end at {2:0} (footer {3})", y, y2, Math.Max(cy, cy2), H - 86);
            }
            Tape(b, 11, new[] { 286, 1046 });
            b.Save(path, ImageFormat.Png);
        }
    }

    static float Card4(Graphics g, string head, string[] lines, float x, float y, float w)
    {
        Header(g, head, x, y, w);
        Font bf = F(24, true);
        float ly = y + 62;
        foreach (string s in lines)
        {
            List<string> wl = Wrap(g, s, bf, w - 34);
            using (SolidBrush b = new SolidBrush(Head)) g.FillRectangle(b, x + 2, ly + 9, 10, 10);
            foreach (string l in wl) { Txt(g, l, bf, Soft, x + 30, ly, 0); ly += bf.Size + 8; }
            ly += 8;
        }
        return ly;
    }

    static void Hands(string path)
    {
        using (Bitmap b = new Bitmap(W, H, PixelFormat.Format32bppArgb))
        {
            using (Graphics g = Begin(b))
            {
                Background(g, 5);
                HudFrame(g, "TAPE 04", "00:00:04:02");
                Title(g, "HANDS ON", F(80, true), 112, 140);
                Txt(g, "TAPE 04 - WHAT YOUR HANDS AND BODY DO", F(24, true), Soft, 116, 236, 0);
                float lx = 112, rx = 990, cw = 820, y0 = 300;
                float a = Card4(g, "THE CAMCORDER", new[] {
                    "Reach to your right hip and squeeze the grip: it comes to your hand.",
                    "Hold it up to your eye to look through it and film.",
                    "Open your hand and it goes back to your hip.",
                    "Place it your way: VR SETTINGS > CAMCORDER." }, lx, y0, cw);
                float c = Card4(g, "DOORS", new[] {
                    "Push a door with your open hand: it swings as far as you push.",
                    "Or grip the handle and swing it. X still opens doors." }, rx, y0, cw);
                float y1 = Math.Max(a, c) + 16;
                float d = Card4(g, "CLIMBING", new[] {
                    "Grip an edge or a ladder rung and pull down: Miles rises.",
                    "Hand over hand. Pull the edge to your chest to climb over.",
                    "Let go of an edge and he drops. On a ladder he holds on - B lets go.",
                    "A still climbs the game's way." }, lx, y1, cw);
                float e = Card4(g, "YOUR BODY", new[] {
                    "Walk around your room: Miles walks with you. The stick works too.",
                    "Lean and peek around corners for real.",
                    "Duck for real to crouch: VR SETTINGS > Crouch when you duck.",
                    "Both stick clicks recenter you." }, rx, y1, cw);
                Footer(g);
                Console.WriteLine("hands: cards end at {0:0} / {1:0} / {2:0} / {3:0} (footer {4})", a, c, d, e, H - 86);
            }
            Tape(b, 23, new[] { 286, 940 });
            b.Save(path, ImageFormat.Png);
        }
    }

    static void Main(string[] args)
    {
        string dir = args.Length > 0 ? args[0] : ".";
        Directory.CreateDirectory(dir);
        Controls(Path.Combine(dir, "Outlast1VR_Controls.png"));
        Hands(Path.Combine(dir, "Outlast1VR_Hands_On.png"));
    }
}
