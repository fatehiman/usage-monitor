// Small anti-aliased shapes drawn by software (4x4 supersampling) and AlphaBlend.
// No GDI+ / Direct2D, so no extra memory.
#pragma once
#include <windows.h>

// pct 0..100 = filled part, clockwise from 12 o'clock. Thin ring in fg color.
void drawPie(HDC dc, int x, int y, int size, double pct, COLORREF fg, COLORREF bg);

// Icon-only button (soft blue tone, circular arrows). angle = rotation in radians (spin).
void drawRefreshButton(HDC dc, int x, int y, int size, double angle, bool hot);

// Icon-only button (soft purple tone, column chart): opens the usage report.
void drawReportButton(HDC dc, int x, int y, int size, bool hot);
