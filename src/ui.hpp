// Shared look for Goals and the Detailed Summary. GDI+ for shapes, GDI for text.
#pragma once
#include <windows.h>
#include <algorithm>
// gdiplus.h needs min/max, which NOMINMAX removes
namespace Gdiplus { using std::min; using std::max; }
#include <objidl.h>
#include <gdiplus.h>
#include <memory>
#include <string>
#include <type_traits>

namespace companion::ui {
struct Palette {
    COLORREF background, card, hover, selected, text, muted, disabled, line, track, box, accent, accent_pressed, on_accent;
    COLORREF gain;
    COLORREF alert;
};
Palette palette();

int dip(HWND window, int value);
float dipf(HWND window, float value);

struct GdiDeleter { void operator()(HGDIOBJ object) const { DeleteObject(object); } };
using FontPtr = std::unique_ptr<std::remove_pointer_t<HFONT>, GdiDeleter>;
FontPtr make_font(HWND window, int points, int weight, bool strike = false);

class GdiplusSession {
public:
    GdiplusSession();
    ~GdiplusSession();
    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;
private:
    ULONG_PTR token_{};
};

class BufferedPaintSession {
public:
    BufferedPaintSession();
    ~BufferedPaintSession();
    BufferedPaintSession(const BufferedPaintSession&) = delete;
    BufferedPaintSession& operator=(const BufferedPaintSession&) = delete;
};

Gdiplus::Color color(COLORREF value);
Gdiplus::RectF rectf(const RECT& r);
void prepare(Gdiplus::Graphics& graphics);
void fill_rounded(Gdiplus::Graphics& graphics, Gdiplus::RectF r, float radius, COLORREF fill);
void stroke_rounded(Gdiplus::Graphics& graphics, Gdiplus::RectF r, float radius, COLORREF stroke, float width);
void draw_text(HDC dc, HFONT font, COLORREF ink, const std::wstring& text, RECT r, UINT format);

enum class ButtonStyle { primary, outline, quiet };
void draw_button(const DRAWITEMSTRUCT& item, ButtonStyle style, HFONT body, HFONT strong);
}
