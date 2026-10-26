#pragma once
// Procedural camcorder artwork in the reference image's 1920x1080 coordinates.
// No game textures. The caller supplies rasterized Arial glyphs (GDI in the mod).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

struct OLCamcorderHudState {
    float energy = 0, zoom = 0, recordingSeconds = 0;
    int spares = 0, capacity = 10;
    bool visible = false;
};
namespace OLCamHud {
constexpr int Width = 1920, Height = 1080, DynamicTop = 0, DynamicHeight = 160;
// The head-locked quad IS the game's display, not a panel floating in the middle of your view:
// the reference artwork is the whole screen, so it has to reach the edges of what you can see.
// A 0.60 m plane 0.45 m away covered only the central ~67 deg, which is what made every readout
// look huge, close, and bunched up in the centre. Sized from the headset's own horizontal FOV
// (PanelWidth) and parked at arm's length, the layout comes out like the reference.
// The quad is head-locked and uses the same picture in both eyes, so it carries no stereo
// disparity: its apparent distance is set by how much of the view it fills, not by how many
// centimetres away it is parked (PanelWidth scales with PanelDistance, so moving it alone changes
// nothing on screen). Pushing it back therefore means filling a little less of the view.
constexpr float PanelDistance = 1.15f;      // metres in front of the eye
constexpr float PanelMargin = 0.97f;        // hair of margin so the corner readouts are not clipped
// Fill the view, but stop before the corners leave the region both eyes can see (~+/-40 deg on a
// Quest): a readout past that is drawn in one eye only and reads as a ghost. 64 deg leaves the REC
// light and the battery gauge at ~28 deg - inside the corners, with clear space around the display,
// which is what reads as set back into the view rather than pressed against the lenses. The
// physical width stays at the reference display's ~1.37 m: the same screen, further out each time
// (84 deg at 0.75 m -> 78 at 0.85 -> 68 at 1.05 -> 64 at 1.15).
constexpr float PanelMinFovDeg = 56.0f, PanelMaxFovDeg = 64.0f;
inline float PanelWidth(float hmdFovDeg) {
    float deg = std::isfinite(hmdFovDeg) ? hmdFovDeg : 0.0f;
    if (deg < PanelMinFovDeg) deg = PanelMinFovDeg;   // no runtime FOV yet, or a narrow headset
    if (deg > PanelMaxFovDeg) deg = PanelMaxFovDeg;   // past this the corners sit in the lens edge
    return 2.0f * PanelDistance * std::tan(deg * 0.5f * 3.14159265358979f / 180.0f) * PanelMargin;
}
inline float Unit(float v) { return std::isfinite(v) && v > 0 ? (v < 1 ? v : 1) : 0; }
// The W-T thumb. Outlast publishes the camcorder zoom as a 0..1 factor; normalising a
// percentage as well keeps a build that reports 0..100 from pinning the thumb at the far end,
// which is exactly what "the W-T bar never changes" looks like.
inline float ZoomUnit(float v) {
    if (!std::isfinite(v) || v <= 0.0f) return 0.0f;
    if (v <= 1.0f) return v;
    return v <= 100.0f ? v * 0.01f : 1.0f;
}
inline void Timecode(float seconds, char (&text)[12]) {
    // The game owns this clock, including pause, lowering the camera and loading saves.
    const double safe = std::isfinite(seconds) && seconds > 0 ? seconds : 0;
    const auto ticks = static_cast<uint64_t>(std::fmod(safe, 360000.0) * 100.0);
    std::snprintf(text, sizeof(text), "%02u:%02u:%02u:%02u",
        unsigned(ticks / 360000), unsigned(ticks / 6000 % 60),
        unsigned(ticks / 100 % 60), unsigned(ticks % 100));
}
struct Canvas {
    std::vector<uint8_t> pixels;
    Canvas() : pixels(size_t(Width) * Height * 4, 0) {}
    void Pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        if (x < 0 || x >= Width || y < 0 || y >= Height) return;
        uint8_t* p = &pixels[(size_t(y) * Width + x) * 4];
        // Straight alpha, matching XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA.
        const unsigned alpha = a + (p[3] * (255 - a) + 127) / 255;
        if (!alpha) return;
        const unsigned old = p[3] * (255 - a);
        p[0] = uint8_t((b * a + (p[0] * old + 127) / 255) / alpha);
        p[1] = uint8_t((g * a + (p[1] * old + 127) / 255) / alpha);
        p[2] = uint8_t((r * a + (p[2] * old + 127) / 255) / alpha);
        p[3] = uint8_t(alpha);
    }
    void Rect(int x, int y, int w, int h, uint8_t shade = 190, uint8_t alpha = 255) {
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx) Pixel(xx, yy, shade, shade, shade, alpha);
    }
    void Rounded(int x, int y, int w, int h, float radius, int top, int bottom) {
        for (int yy = 0; yy < h; ++yy) for (int xx = 0; xx < w; ++xx) {
            const float dx = (std::max)(radius - xx - 0.5f, xx + 0.5f - (w - radius));
            const float dy = (std::max)(radius - yy - 0.5f, yy + 0.5f - (h - radius));
            const float edge = radius - std::sqrt((std::max)(0.0f,dx)*(std::max)(0.0f,dx) +
                                                 (std::max)(0.0f,dy)*(std::max)(0.0f,dy));
            const auto shade = uint8_t(top + (bottom-top)*yy/(std::max)(1,h-1));
            Pixel(x+xx,y+yy,shade,shade,shade,radius > 0 ? uint8_t(Unit(edge+0.5f)*255) : 255);
        }
    }
    void Outline(int x, int y, int w, int h, int t, uint8_t shade = 190) {
        Rect(x,y,w,t,shade); Rect(x,y+h-t,w,t,shade);
        Rect(x,y,t,h,shade); Rect(x+w-t,y,t,h,shade);
    }
};
// text(canvas, x, y, pixelHeight, bold, string, red, green, blue)
// Static artwork at the reference screenshot's own 1920x1080 positions: the lens markings sit in
// the bottom-left corner of the display, the focusing brackets in the middle of it.
template<class Text> void Static(Canvas& c, Text& text) {
    // Focusing brackets - the one bright shape in the middle of the view, and the only one with
    // nothing behind it, so it reads as a square hanging in front of the face. The reference's
    // 354x298 box was too big for that: 212x178 (60%) on the same centre (960.5, 550.5), arms 56 px.
    // Slightly translucent, but only just: the compositor on at least one runtime multiplies the
    // layer's colour by its own alpha even though the layer asks for XR_COMPOSITION_LAYER_UNPREMULTIPLIED
    // (measured: 175 at 200/255 came out black). Keeping the alpha high means a double multiply still
    // lands on light grey (~161) instead of a dark line, while the picture behind shows through a little.
    const uint8_t bracket = 190, bracketAlpha = 235;
    c.Rect(855,462,56,2,bracket,bracketAlpha); c.Rect(1011,462,56,2,bracket,bracketAlpha);
    c.Rect(855,464,2,174,bracket,bracketAlpha); c.Rect(1065,464,2,174,bracket,bracketAlpha);
    c.Rect(855,638,56,2,bracket,bracketAlpha); c.Rect(1011,638,56,2,bracket,bracketAlpha);
    c.Rounded(149,819,108,52,4,215,170);
    for (int y = 823; y <= 855; y += 16) {
        c.Rounded(153,y,14,12,3,0,0); c.Rounded(239,y,14,12,3,0,0);
    }
    text(c,172,821,44,true,"HD",0,0,0);
    c.Rounded(149,891,138,54,5,221,170);
    text(c,157,893,44,true,"AUTO",0,0,0);
    text(c,146,955,48,true,"F2.4",200,200,200);
}
template<class Text> void Top(Canvas& c, const OLCamcorderHudState& s, Text& text) {
    // Only the top readout band changes; the brackets and the lens markings below it stay put.
    // Exactly the rows the upload refreshes (DynamicTop/DynamicHeight), so the two cannot drift.
    std::fill(c.pixels.begin() + size_t(Width)*DynamicTop*4,
              c.pixels.begin() + size_t(Width)*(DynamicTop+DynamicHeight)*4, 0);
    // Every readout sits a step further out and up than the reference's own coordinates - 80 px further
    // out and 10 px up. The quad now fills less of the view (PanelWidth), so the display's corners land
    // nearer the middle of what you see; the clusters are pushed toward the edges so they still read as
    // corners, with a wider gap left around the centred W-T lever. The focusing brackets are untouched.
    // Record light and REC, top-left corner of the display.
    for (int y = 80; y < 120; ++y) for (int x = 34; x < 75; ++x) {
        const float d = std::sqrt((x-54.5f)*(x-54.5f) + (y-99.5f)*(y-99.5f));
        const float coverage = Unit(19.5f-d);
        c.Pixel(x,y,255,0,0,uint8_t(coverage*255));
    }
    text(c,79,72,46,true,"REC",255,0,0);
    char time[12]; Timecode(s.recordingSeconds, time);
    text(c,230,80,34,true,time,255,0,0);
    // Zoom lever: W (wide) on the left, T (tele) on the right, centred in the display.
    text(c,747,78,44,false,"W",200,200,200);
    text(c,1117,78,44,false,"T",200,200,200);
    c.Rounded(810,90,287,28,1,116,80); c.Outline(810,90,287,28,2,200);
    // The thumb is the game's own zoom factor: dark outline, bright body, so any move shows.
    const int tracker = 813 + int(ZoomUnit(s.zoom)*260);
    c.Rect(tracker,92,16,23,25); c.Rect(tracker+2,94,12,19,225);
    // Spare batteries, then the charge gauge, top-right corner of the display.
    const int capacity = s.capacity > 0 ? s.capacity : 10;
    const int spares = s.spares < 0 ? 0 : (s.spares > capacity ? capacity : s.spares);
    char count[32]; std::snprintf(count,sizeof(count),"%d / %d",spares,capacity);
    // Night vision lifts the scene behind the HUD to a bright green wash, where a light grey readout
    // has no edge left to read against and simply disappears. A one-pixel black rim under the count
    // and around the gauge gives both their outline back without making them heavier. Four neighbours
    // rather than eight: the count is redrawn every frame and each extra pass is a full glyph blit.
    const int rimX[4] = { -1, 1, 0, 0 }, rimY[4] = { 0, 0, -1, 1 };
    for (int i = 0; i < 4; ++i) text(c, 1584 + rimX[i], 81 + rimY[i], 25, true, count, 0, 0, 0);
    text(c,1584,81,25,true,count,210,210,210);
    // Terminal faces left, as in the supplied reference. Continuous charge, no 4-bar quantization.
    c.Outline(1677,84,13,24,2,0);                       // rim around the terminal tab
    c.Rect(1679,86,9,20,200);                           // the terminal itself
    c.Outline(1684,70,178,52,2,0);                      // rim around the gauge body
    c.Outline(1686,72,170,48,5,200);                    // the gauge body
    c.Rect(1684,91,5,10,0);
    const float charge = Unit(s.energy);
    // Preserve the large gauge's low-charge warning (one bar or less), with a continuous fill.
    const bool low = std::ceil(charge*4.0f-0.05f) <= 1;
    for (int y = 0; y < 24; ++y) for (int x = 0; x < int(142*charge); ++x) {
        const uint8_t shade = uint8_t(215-35*y/23);
        c.Pixel(1703+x,84+y,low ? 255 : shade,low ? 60 : shade,low ? 60 : shade);
    }
}
}
