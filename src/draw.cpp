#include "draw.h"
#include <cmath>
#include <cstdint>

namespace {

const double PI = 3.14159265358979323846;

struct C4 { double r, g, b, a; };

C4 rgb(COLORREF c) { return {(double)GetRValue(c), (double)GetGValue(c), (double)GetBValue(c), 1}; }
const C4 NONE = {0, 0, 0, 0};

// shade(u, v) gets pixel coordinates (with fractions) and returns the color there.
template <class F>
void blit(HDC dc, int x, int y, int w, int h, F shade) {
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return;
    uint32_t* px = (uint32_t*)bits;
    const int N = 4;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < N; sy++)
                for (int sx = 0; sx < N; sx++) {
                    C4 c = shade(i + (sx + 0.5) / N, j + (sy + 0.5) / N);
                    r += c.r * c.a; g += c.g * c.a; b += c.b * c.a; a += c.a;
                }
            const double k = 1.0 / (N * N);  // premultiplied average
            px[j * w + i] = ((uint32_t)(a * k * 255 + 0.5) << 24) | ((uint32_t)(r * k + 0.5) << 16) |
                            ((uint32_t)(g * k + 0.5) << 8) | (uint32_t)(b * k + 0.5);
        }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, bmp);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, x, y, w, h, mem, 0, 0, w, h, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(bmp);
}

double segDist(double px, double py, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay;
    double t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

bool inRoundRect(double u, double v, double cx, double cy, double half, double rr) {
    double qx = std::fabs(u - cx) - (half - rr), qy = std::fabs(v - cy) - (half - rr);
    if (qx > rr || qy > rr) return false;
    qx = qx < 0 ? 0 : qx;
    qy = qy < 0 ? 0 : qy;
    return std::hypot(qx, qy) <= rr;
}

}  // namespace

void drawPie(HDC dc, int x, int y, int size, double pct, COLORREF fg, COLORREF bg) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    const double c = size / 2.0, R = size / 2.0 - 0.25, ring = size >= 18 ? 1.4 : 1.1;
    const double end = pct / 100 * 2 * PI;
    const C4 f = rgb(fg), b = rgb(bg);
    blit(dc, x, y, size, size, [&](double u, double v) -> C4 {
        double dx = u - c, dy = v - c, r = std::hypot(dx, dy);
        if (r > R) return NONE;
        if (r > R - ring) return f;
        double a = std::atan2(dx, -dy);  // 0 at 12 o'clock, clockwise
        if (a < 0) a += 2 * PI;
        return a <= end ? f : b;
    });
}

void drawRefreshButton(HDC dc, int x, int y, int size, double angle, bool hot) {
    // Tone "blue" from the icon-buttons rule (light mode).
    const C4 icon = rgb(RGB(0x3b, 0x64, 0xb5));
    const C4 bg = rgb(hot ? RGB(0xdb, 0xe6, 0xfa) : RGB(0xe8, 0xef, 0xfc));
    const C4 border = rgb(RGB(0xc9, 0xd8, 0xf4));
    const double half = size / 2.0, rr = size * 0.2;
    const double iconPx = size * 0.72, off = (size - iconPx) / 2, k = 24.0 / iconPx;
    const double ca = std::cos(-angle), sa = std::sin(-angle);
    const double arcEnd = 312 * PI / 180;  // Lucide "refresh": arc from 0 deg to 312 deg (y down)
    const double ex = 12 + 9 * std::cos(arcEnd), ey = 12 + 9 * std::sin(arcEnd);
    blit(dc, x, y, size, size, [&](double u, double v) -> C4 {
        if (!inRoundRect(u, v, half, half, half, rr)) return NONE;
        // to the 24x24 icon grid, rotated around its center
        double gx = (u - off) * k - 12, gy = (v - off) * k - 12;
        double rx = gx * ca - gy * sa + 12, ry = gx * sa + gy * ca + 12;
        double t = std::atan2(ry - 12, rx - 12);
        if (t < 0) t += 2 * PI;
        double d = t <= arcEnd ? std::fabs(std::hypot(rx - 12, ry - 12) - 9)
                               : std::fmin(std::hypot(rx - 21, ry - 12), std::hypot(rx - ex, ry - ey));
        d = std::fmin(d, segDist(rx, ry, 21, 3, 21, 9));
        d = std::fmin(d, segDist(rx, ry, 21, 9, 15, 9));
        if (d <= 1.15) return icon;  // stroke width 2 (a little more: small sizes)
        return inRoundRect(u, v, half, half, half - 1, rr - 1 > 0 ? rr - 1 : 0) ? bg : border;
    });
}

void drawReportButton(HDC dc, int x, int y, int size, bool hot) {
    // Tone "purple" (history / info) from the icon-buttons rule (light mode).
    const C4 icon = rgb(RGB(0x6c, 0x52, 0xa8));
    const C4 bg = rgb(hot ? RGB(0xe4, 0xdc, 0xf6) : RGB(0xef, 0xea, 0xfa));
    const C4 border = rgb(RGB(0xdc, 0xd2, 0xf1));
    const double half = size / 2.0, rr = size * 0.2;
    const double iconPx = size * 0.72, off = (size - iconPx) / 2, k = 24.0 / iconPx;
    blit(dc, x, y, size, size, [&](double u, double v) -> C4 {
        if (!inRoundRect(u, v, half, half, half, rr)) return NONE;
        double gx = (u - off) * k, gy = (v - off) * k;  // 24x24 icon grid
        // Lucide "chart-column": axis + 3 bars.
        double d = segDist(gx, gy, 3, 3, 3, 21);
        d = std::fmin(d, segDist(gx, gy, 3, 21, 21, 21));
        d = std::fmin(d, segDist(gx, gy, 8, 17, 8, 14));
        d = std::fmin(d, segDist(gx, gy, 13, 17, 13, 5));
        d = std::fmin(d, segDist(gx, gy, 18, 17, 18, 9));
        if (d <= 1.15) return icon;
        return inRoundRect(u, v, half, half, half - 1, rr - 1 > 0 ? rr - 1 : 0) ? bg : border;
    });
}
