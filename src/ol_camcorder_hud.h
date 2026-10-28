#pragma once
// Procedural camcorder artwork in the reference image's 1920x1080 coordinates.
// No game textures. The caller supplies rasterized Arial glyphs (GDI in the mod).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

struct OLCamcorderHudState {
    float energy = 0, zoom = 0, recordingSeconds = 0;
    int spares = 0, capacity = 10;
    bool nightVision = false;       // the camcorder's own night vision is running: the eye and speaker show
    bool visible = false;
};
namespace OLCamHud {
// ---- palette --------------------------------------------------------------------------------------
// Two inks, straight from the reference artwork. #A71000 is the recording red - the REC lamp and its
// caption, the timecode and the spare-battery count. #808079 is the neutral grey of everything else
// that is drawn: the lever, the charge gauge, the badges and the viewfinder marks (and the brackets,
// which read as one thin light line rather than the bright white box they were). Both are drawn
// opaque, so the per-eye blend cannot shift them the way a translucent grey once came out black.
constexpr uint8_t RecRed = 167, RecGreen = 16, RecBlue = 0;        // #A71000
constexpr uint8_t IconRed = 128, IconGreen = 128, IconBlue = 121;  // #808079

constexpr int Width = 1920, Height = 1080, DynamicTop = 0, DynamicHeight = 160;
// The head-locked quad IS the game's display, not a panel floating in the middle of your view:
// the reference artwork is the whole screen, so it has to reach the edges of what you can see.
// A 0.60 m plane 0.45 m away covered only the central ~67 deg, which is what made every readout
// look huge, close, and bunched up in the centre. Sized from the headset's own horizontal FOV
// (PanelWidth) and parked at arm's length, the layout comes out like the reference.
// PanelDistance is the display's STEREO DEPTH, not just its size, and that is why it is 2 m and
// not 1. A quad layer submitted in the VIEW space is composited into EACH eye from that eye's own
// head-relative pose, so a plane d metres ahead asks the eyes to converge on d (the picture is a
// single texture, but the two projections are not identical). That is the spec's model: a quad
// layer is "a posable planar rectangle in the virtual world", and the VIEW space is the centroid of
// the two eye origins in stereo, not an eye. An earlier revision called the quad
// "mono, so it has no stereo disparity" and treated the distance as a size-only knob; that is not
// how a quad layer is composited, and it is exactly what "the focusing brackets read double" was.
// The brackets are the one thin, bright, dead-centre shape, and the middle of a near plane is where
// its disparity is largest: ~3.4 deg at 1.05 m. While the player's eyes are converged on the room
// behind the display, the two copies cannot fuse and read as two squares - not always, only when
// the scene is further away than the display (which is most of the time).
// 2.00 m asks for ~1.8 deg, about what a corridor or an ordinary room asks for anyway, so the box
// fuses with the scene instead of hanging in front of it (3.4 deg at 1.05 m, 2.4 at 1.5, 1.8 at
// 2.0, 1.4 at 2.5). PanelWidth scales with PanelDistance, so the display covers exactly the same
// part of the view as before - only the depth the eyes are asked for changes, which no monitor can
// show. If the brackets ever read double again, move this further out, never back to the lenses.
constexpr float PanelDistance = 2.00f;      // metres in front of the eye
constexpr float PanelMargin = 0.97f;        // hair of margin so the corner readouts are not clipped
// Fill the view, but stop short of the lens edge, where a readout smears and pulls the eye away
// from the picture. 68 deg leaves the REC light and the battery gauge at ~32 deg - inside the
// corners, with clear space around the display, which is ~2.6 m wide at the 2 m parking distance.
// (PanelWidth scales with PanelDistance, so this clamp alone sets the fill, not the depth.)
constexpr float PanelMinFovDeg = 60.0f, PanelMaxFovDeg = 68.0f;
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
    // Flat RGB fills. The display has two inks (the recording red and the HUD grey), so the greys-only
    // Rect/Rounded/Outline helpers the artwork used to be built from are gone with them.
    void Fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx) Pixel(xx, yy, r, g, b);
    }
    // A real hole: alpha 0, not black. The layer is drawn over the scene, so "transparent" and a
    // black plate look nothing alike in the headset. radius > 0 rounds the corners.
    void Clear(int x, int y, int w, int h, float radius = 0) {
        for (int yy = 0; yy < h; ++yy) for (int xx = 0; xx < w; ++xx) {
            if (radius > 0) {
                const float dx = (std::max)(radius - xx - 0.5f, xx + 0.5f - (w - radius));
                const float dy = (std::max)(radius - yy - 0.5f, yy + 0.5f - (h - radius));
                const float edge = radius - std::sqrt((std::max)(0.0f,dx)*(std::max)(0.0f,dx) +
                                                      (std::max)(0.0f,dy)*(std::max)(0.0f,dy));
                if (edge < 0.0f) continue;
            }
            const int px = x + xx, py = y + yy;
            if (px < 0 || px >= Width || py < 0 || py >= Height) continue;
            uint8_t* p = &pixels[(size_t(py) * Width + px) * 4];
            p[0] = p[1] = p[2] = p[3] = 0;
        }
    }
    // Filled rounded rectangle in an RGB colour (radius 0 = square corners).
    void RoundFill(int x, int y, int w, int h, float radius, uint8_t r, uint8_t g, uint8_t b) {
        for (int yy = 0; yy < h; ++yy) for (int xx = 0; xx < w; ++xx) {
            float a = 1.0f;
            if (radius > 0) {
                const float dx = (std::max)(radius - xx - 0.5f, xx + 0.5f - (w - radius));
                const float dy = (std::max)(radius - yy - 0.5f, yy + 0.5f - (h - radius));
                const float edge = radius - std::sqrt((std::max)(0.0f,dx)*(std::max)(0.0f,dx) +
                                                      (std::max)(0.0f,dy)*(std::max)(0.0f,dy));
                a = Unit(edge + 0.5f);
            }
            if (a > 0) Pixel(x+xx, y+yy, r, g, b, uint8_t(a*255));
        }
    }
};
// text(canvas, x, y, pixelHeight, bold, string, red, green, blue)
// Static artwork at the reference screenshot's own 1920x1080 positions: the lens markings sit in
// the bottom-left corner of the display, the focusing brackets in the middle of it.
template<class Text> void Static(Canvas& c, Text& text) {
    // Focusing brackets - the one shape in the middle of the view with nothing behind it, so it reads
    // as a square hanging in front of the face. Kept small and made FINER: 167x139 (was 212x178) on
    // the same centre (960.5, 550.5) with 44 px arms, one pixel thin, drawn in the HUD grey (#808079)
    // instead of a bright 175. It is the thing you aim the camcorder with, so the arms stay long
    // enough to read as a box, but nothing about it is allowed to pull the eye off the picture.
    // Translucency is not usable here: the compositor multiplies the layer's colour by its own alpha
    // even though the layer asks for XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT, and what should
    // have been a soft grey box came out black at 200/255 and still dark at 235/255. Alpha stays 255.
    const int bx = 877, by = 481, bw = 167, bh = 139, arm = 44;
    auto bar = [&](int x, int y, int w, int h) { c.Fill(x, y, w, h, IconRed, IconGreen, IconBlue); };
    bar(bx, by, arm, 1);            bar(bx+bw-arm, by, arm, 1);
    bar(bx, by+bh-1, arm, 1);       bar(bx+bw-arm, by+bh-1, arm, 1);
    bar(bx, by+1, 1, bh-2);         bar(bx+bw-1, by+1, 1, bh-2);
    // Lens markings, bottom-left corner, in the same grey. The HD badge's perforations are HOLES:
    // the display is transparent over the scene, so the squares were never meant to be black plates.
    c.RoundFill(149, 819, 108, 52, 4.0f, IconRed, IconGreen, IconBlue);
    for (int y = 823; y <= 855; y += 16) {
        c.Clear(153, y, 14, 12, 3.0f); c.Clear(239, y, 14, 12, 3.0f);
    }
    text(c,172,821,44,true,"HD",0,0,0);
    c.RoundFill(149, 891, 138, 54, 5.0f, IconRed, IconGreen, IconBlue);
    text(c,157,893,44,true,"AUTO",0,0,0);
    text(c,146,955,48,true,"F2.4",IconRed,IconGreen,IconBlue);
}
// The two night-vision tell-tales under the right end of the gauge: the eye and the speaker from the
// reference artwork. They are drawn only while the camcorder's night vision is running, so the
// display itself says which mode it is in. Both live inside the dynamic band, which is cleared every
// frame, so switching night vision off takes them away again.
inline void NightVisionIcons(Canvas& c) {
    const int iconY = 106, iconH = 40, iconW = 62;   // just under the gauge, which now ends at y=99
    const int speakerX = 1848 - iconW;          // right-aligned with the gauge's right edge
    const int eyeX = speakerX - 8 - iconW;
    for (int x : { eyeX, speakerX }) {          // the two frames: a rounded outline, hollow inside
        c.RoundFill(x, iconY, iconW, iconH, 3.0f, IconRed, IconGreen, IconBlue);
        c.Clear(x+2, iconY+2, iconW-4, iconH-4, 2.0f);
    }
    // Eye: the almond (two circle arcs meeting at the corners), its iris ring and the pupil.
    const float ex = eyeX + iconW * 0.5f, ey = iconY + iconH * 0.5f;
    const float a = 19.0f, b = 10.5f;
    const float lobeR = (a*a + b*b) / (2.0f * b), lobeY = lobeR - b;
    for (int y = int(ey-b)-1; y <= int(ey+b)+1; ++y) for (int x = int(ex-a)-1; x <= int(ex+a)+1; ++x) {
        const float dx = x + 0.5f - ex, dy = y + 0.5f - ey;
        if (std::fabs(dx) > a) continue;
        // Distance from the eye's centre line to the almond's edge, both lobes alike.
        const float edge = std::fabs(lobeY - std::sqrt(lobeR*lobeR - dx*dx));
        if (std::fabs(dy) >= edge && std::fabs(dy) <= edge + 1.8f)   // a ~2 px stroke
            c.Pixel(x, y, IconRed, IconGreen, IconBlue);
    }
    auto disc = [&](float cx, float cy, float r) {
        for (int y = int(cy-r)-1; y <= int(cy+r)+1; ++y) for (int x = int(cx-r)-1; x <= int(cx+r)+1; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (std::sqrt(dx*dx + dy*dy) <= r) c.Pixel(x, y, IconRed, IconGreen, IconBlue);
        }
    };
    auto clearDisc = [&](float cx, float cy, float r) {
        for (int y = int(cy-r)-1; y <= int(cy+r)+1; ++y) for (int x = int(cx-r)-1; x <= int(cx+r)+1; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (std::sqrt(dx*dx + dy*dy) <= r) c.Clear(x, y, 1, 1);
        }
    };
    disc(ex, ey, 7.0f); clearDisc(ex, ey, 4.4f); disc(ex, ey, 2.6f);
    // Speaker: the cone, then the sound waves.
    const float sx = speakerX + iconW * 0.5f - 1.0f;
    c.Fill(int(sx-16), int(ey-7), 9, 15, IconRed, IconGreen, IconBlue);
    for (int x = int(sx-7); x < int(sx+3); ++x) {
        const float t = (x - (sx-7)) / 10.0f;
        const int half = int(7.0f + t * 7.5f + 0.5f);
        c.Fill(x, int(ey)-half, 1, half*2, IconRed, IconGreen, IconBlue);
    }
    for (float r : { 6.0f, 11.0f, 16.0f })                            // three arcs, ~2 px thick
        for (int y = int(ey-r-2); y <= int(ey+r+2); ++y)
            for (int x = int(sx); x <= int(sx+r+2); ++x) {
                const float dx = x + 0.5f - sx, dy = y + 0.5f - ey;
                if (std::fabs(std::sqrt(dx*dx + dy*dy) - r) > 1.0f) continue;
                if (std::fabs(std::atan2(dy, dx)) * 57.29578f > 50.0f) continue;
                c.Pixel(x, y, IconRed, IconGreen, IconBlue);
            }
}

template<class Text> void Top(Canvas& c, const OLCamcorderHudState& s, Text& text) {
    // Only the top readout band changes; the brackets and the lens markings below it stay put.
    // Exactly the rows the upload refreshes (DynamicTop/DynamicHeight), so the two cannot drift.
    std::fill(c.pixels.begin() + size_t(Width)*DynamicTop*4,
              c.pixels.begin() + size_t(Width)*(DynamicTop+DynamicHeight)*4, 0);
    // Every readout sits a step further out and higher than the reference's own coordinates - 80 px out,
    // 20 px up - because the quad fills less of the view (PanelWidth), so the display's corners land
    // nearer the middle of what you see. The clusters are pushed toward the edges and the top, which
    // keeps them reading as corners and leaves the focusing brackets below them clear.
    // Record light and REC, top-left corner of the display, in the recording red (#A71000).
    for (int y = 60; y < 100; ++y) for (int x = 34; x < 75; ++x) {
        const float d = std::sqrt((x-54.5f)*(x-54.5f) + (y-79.5f)*(y-79.5f));
        const float coverage = Unit(19.5f-d);
        c.Pixel(x,y,RecRed,RecGreen,RecBlue,uint8_t(coverage*255));
    }
    text(c,79,52,46,true,"REC",RecRed,RecGreen,RecBlue);
    char time[12]; Timecode(s.recordingSeconds, time);
    text(c,230,60,34,true,time,RecRed,RecGreen,RecBlue);
    // Zoom lever: W (wide) on the left, T (tele) on the right, centred in the display.
    text(c,747,58,44,false,"W",IconRed,IconGreen,IconBlue);
    text(c,1117,58,44,false,"T",IconRed,IconGreen,IconBlue);
    c.RoundFill(810,70,287,28,1.0f,IconRed,IconGreen,IconBlue);   // the lever's frame
    c.RoundFill(812,72,283,24,0.0f,52,52,49);                     // ...and its dark track
    // The thumb is the game's own zoom factor: dark outline, grey body, so any move shows.
    const int tracker = 813 + int(ZoomUnit(s.zoom)*260);
    c.Fill(tracker,72,16,23,36,36,33); c.Fill(tracker+2,74,12,19,IconRed,IconGreen,IconBlue);
    // Spare batteries, then the charge gauge, top-right corner of the display.
    const int capacity = s.capacity > 0 ? s.capacity : 10;
    const int spares = s.spares < 0 ? 0 : (s.spares > capacity ? capacity : s.spares);
    char count[32]; std::snprintf(count,sizeof(count),"%d / %d",spares,capacity);
    // The count sits immediately left of the contact, in the HUD grey like the gauge it belongs to -
    // the recording red is for the record light and the clock only.
    // No dark rim: the display is a transparent layer over the scene, so a black outline around a
    // readout is a black line drawn on the picture, not a shadow.
    text(c,1524,61,25,true,count,IconRed,IconGreen,IconBlue);
    // The gauge: the reference's long, thin cell, contact on the LEFT as a bracket that opens into the
    // body, drawn as a rounded frame with a transparent window - no black body behind it, no rim.
    const int gaugeX = 1636, gaugeY = 52, gaugeW = 212, gaugeH = 48;
    c.RoundFill(gaugeX, gaugeY, gaugeW, gaugeH, 7.0f, IconRed, IconGreen, IconBlue);
    c.Clear(gaugeX+4, gaugeY+4, gaugeW-8, gaugeH-8, 4.0f);
    // Contact: a bracket that opens into the body, exactly as in the reference.
    c.RoundFill(gaugeX-18, gaugeY+9, 18, 30, 3.0f, IconRed, IconGreen, IconBlue);
    c.Clear(gaugeX-9, gaugeY+18, 9, 12);
    const int barX = gaugeX + 10, barY = gaugeY + 10, barW = gaugeW - 20, barH = gaugeH - 20;
    const float charge = Unit(s.energy);
    // Preserve the large gauge's low-charge warning (one bar or less), with a continuous fill.
    const bool low = std::ceil(charge*4.0f-0.05f) <= 1;
    // Continuous charge, no 4-bar quantization, and it DRAINS LEFT TO RIGHT: the charge leaves at the
    // contact end, so what is left of the bar sits against the far (right) end of the cell.
    const int filled = int(barW * charge + 0.5f);
    for (int y = 0; y < barH; ++y) {
        const int drop = 12 - 26*y/(barH-1);                        // the reference's soft vertical fall
        for (int x = 0; x < filled; ++x) {
            if (low) c.Pixel(barX+barW-filled+x, barY+y, RecRed, RecGreen, RecBlue);
            else c.Pixel(barX+barW-filled+x, barY+y,
                         uint8_t(IconRed+drop), uint8_t(IconGreen+drop), uint8_t(IconBlue+drop));
        }
    }
    // Night vision is running: the eye and the speaker appear under the gauge's right end.
    if (s.nightVision) NightVisionIcons(c);
}
}
