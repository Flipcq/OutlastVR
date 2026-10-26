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
constexpr int Width = 1920, Height = 1080, DynamicTop = 240, DynamicHeight = 160;
// Compact, enlarged readouts inside the central field of view. Keep the dynamic band
// above the reticle (y=402) so updates cannot erase the static artwork below it.
constexpr float PanelDistance = 0.45f, PanelWidth = 0.60f;
inline float Unit(float v) { return std::isfinite(v) && v > 0 ? (v < 1 ? v : 1) : 0; }
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
    void Rect(int x, int y, int w, int h, uint8_t shade = 190) {
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx) Pixel(xx, yy, shade, shade, shade);
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
template<class Text> void Static(Canvas& c, Text& text) {
    c.Rect(784,402,93,2); c.Rect(784,402,2,298); c.Rect(784,698,93,2);
    c.Rect(1044,402,94,2); c.Rect(1136,402,2,298); c.Rect(1044,698,94,2);
    c.Rounded(430,660,146,70,5,215,170);
    for (int y = 666; y <= 710; y += 22) {
        c.Rounded(436,y,19,16,4,0,0); c.Rounded(552,y,19,16,4,0,0);
    }
    text(c,461,663,60,true,"HD",0,0,0);
    c.Rounded(430,754,186,73,6,221,170);
    text(c,441,758,60,true,"AUTO",0,0,0);
    text(c,426,846,64,true,"F2.4",200,200,200);
}
template<class Text> void Top(Canvas& c, const OLCamcorderHudState& s, Text& text) {
    std::fill(c.pixels.begin() + size_t(Width)*DynamicTop*4,
              c.pixels.begin() + size_t(Width)*(DynamicTop+DynamicHeight)*4, 0);
    for (int y = 253; y < 308; ++y) for (int x = 417; x < 472; ++x) {
        const float d = std::sqrt((x-444.0f)*(x-444.0f) + (y-280.0f)*(y-280.0f));
        const float coverage = Unit(26.5f-d);
        c.Pixel(x,y,255,0,0,uint8_t(coverage*255));
    }
    text(c,480,244,64,true,"REC",255,0,0);
    char time[12]; Timecode(s.recordingSeconds, time);
    text(c,418,326,48,true,time,255,0,0);
    text(c,734,249,58,false,"W",200,200,200);
    text(c,1140,249,58,false,"T",200,200,200);
    c.Rounded(810,267,306,40,1,116,80); c.Outline(810,267,306,40,2,200);
    const int tracker = 813 + int(Unit(s.zoom)*278);
    c.Rect(tracker,270,20,33,15); c.Rect(tracker+3,273,14,27,210);
    const int capacity = s.capacity > 0 ? s.capacity : 10;
    const int spares = s.spares < 0 ? 0 : (s.spares > capacity ? capacity : s.spares);
    char count[32]; std::snprintf(count,sizeof(count),"%d / %d",spares,capacity);
    text(c,1300,329,38,true,count,210,210,210);
    // Terminal faces left, as in the supplied reference. Continuous charge, no 4-bar quantization.
    c.Rect(1242,274,14,28,200); c.Outline(1252,254,240,68,6,200);
    c.Rect(1248,280,7,16,0);
    const float charge = Unit(s.energy);
    // Preserve the large gauge's low-charge warning (one bar or less), with a continuous fill.
    const bool low = std::ceil(charge*4.0f-0.05f) <= 1;
    for (int y = 0; y < 34; ++y) for (int x = 0; x < int(192*charge); ++x) {
        const uint8_t shade = uint8_t(215-35*y/33);
        c.Pixel(1276+x,271+y,low ? 255 : shade,low ? 60 : shade,low ? 60 : shade);
    }
}
}
