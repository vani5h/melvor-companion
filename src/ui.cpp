#include "ui.hpp"
#include "settings.hpp"
#include <uxtheme.h>
#include <stdexcept>

namespace companion::ui {
Palette palette() {
    if (dark_mode())
        return {RGB(24,24,24), RGB(31,31,31), RGB(38,40,39), RGB(44,48,46), RGB(240,242,241), RGB(139,146,143),
                RGB(88,94,91), RGB(51,54,52), RGB(44,46,45), RGB(122,129,126), RGB(135,219,188), RGB(116,199,169), RGB(15,40,30),
                RGB(135,219,188), RGB(240,84,84)};
    return {RGB(247,248,248), RGB(255,255,255), RGB(238,242,241), RGB(230,236,234), RGB(26,28,28), RGB(107,115,113),
            RGB(170,176,174), RGB(218,223,221), RGB(227,232,230), RGB(138,146,144), RGB(135,219,188), RGB(116,199,169), RGB(15,40,30),
            RGB(22,128,92), RGB(214,52,52)};
}

int dip(HWND window, int value) { return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96); }
float dipf(HWND window, float value) { return value * static_cast<float>(GetDpiForWindow(window)) / 96.0f; }

FontPtr make_font(HWND window, int points, int weight, bool strike) {
    LOGFONTW font{};
    font.lfHeight = -MulDiv(points, static_cast<int>(GetDpiForWindow(window)), 72);
    font.lfWeight = weight;
    font.lfStrikeOut = strike;
    font.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    return FontPtr(CreateFontIndirectW(&font));
}

GdiplusSession::GdiplusSession() {
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token_, &input, nullptr) != Gdiplus::Ok) throw std::runtime_error("Could not start GDI+");
}
GdiplusSession::~GdiplusSession() { Gdiplus::GdiplusShutdown(token_); }
BufferedPaintSession::BufferedPaintSession() { BufferedPaintInit(); }
BufferedPaintSession::~BufferedPaintSession() { BufferedPaintUnInit(); }

Gdiplus::Color color(COLORREF value) { return Gdiplus::Color(255, GetRValue(value), GetGValue(value), GetBValue(value)); }
Gdiplus::RectF rectf(const RECT& r) {
    return Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                          static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
}
void prepare(Gdiplus::Graphics& graphics) {
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // Pixel centers sit at .5, so shapes on whole coordinates cover whole pixels and stay crisp.
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}
namespace {
void rounded_path(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius) {
    float d = std::min(radius * 2, std::min(r.Width, r.Height));
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}
}
void fill_rounded(Gdiplus::Graphics& graphics, Gdiplus::RectF r, float radius, COLORREF fill) {
    Gdiplus::GraphicsPath path;
    rounded_path(path, r, radius);
    Gdiplus::SolidBrush brush(color(fill));
    graphics.FillPath(&brush, &path);
}
void stroke_rounded(Gdiplus::Graphics& graphics, Gdiplus::RectF r, float radius, COLORREF stroke, float width) {
    // Inset by half the pen width so the line stays inside r.
    r.Inflate(-width / 2, -width / 2);
    Gdiplus::GraphicsPath path;
    rounded_path(path, r, radius);
    Gdiplus::Pen pen(color(stroke), width);
    graphics.DrawPath(&pen, &path);
}
void draw_text(HDC dc, HFONT font, COLORREF ink, const std::wstring& text, RECT r, UINT format) {
    auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);
    DrawTextW(dc, text.c_str(), -1, &r, format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, old);
}

void draw_button(const DRAWITEMSTRUCT& item, ButtonStyle style, HFONT body, HFONT strong) {
    auto p = palette();
    HWND parent = GetParent(item.hwndItem);
    bool disabled = (item.itemState & ODS_DISABLED) != 0;
    bool pressed = (item.itemState & ODS_SELECTED) != 0;
    bool focus_ring = (item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT);
    HDC dc = item.hDC;
    RECT r = item.rcItem;
    // Paint the whole rectangle first, so no stale pixels show around the rounded corners.
    SetDCBrushColor(dc, p.background);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    float radius = dipf(parent, 8);
    COLORREF ink = p.text;
    HFONT font = body;
    UINT align = DT_CENTER;
    {
        Gdiplus::Graphics graphics(dc);
        prepare(graphics);
        if (style == ButtonStyle::primary) {
            fill_rounded(graphics, rectf(r), radius, disabled ? p.track : pressed ? p.accent_pressed : p.accent);
            ink = disabled ? p.disabled : p.on_accent;
            font = strong;
        } else if (style == ButtonStyle::outline) {
            if (pressed) fill_rounded(graphics, rectf(r), radius, p.hover);
            stroke_rounded(graphics, rectf(r), radius, p.line, dipf(parent, 1));
        } else {
            if (pressed) fill_rounded(graphics, rectf(r), radius, p.hover);
            ink = disabled ? p.disabled : p.muted;
            align = DT_LEFT;
        }
        if (focus_ring) stroke_rounded(graphics, rectf(r), radius, p.accent, dipf(parent, 1.5f));
    }
    wchar_t label[64]{};
    GetWindowTextW(item.hwndItem, label, 64);
    if (align == DT_LEFT) r.left += dip(parent, 6);
    draw_text(dc, font, ink, label, r, align);
}
}
