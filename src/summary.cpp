// The Detailed Summary window: what changed while the game was hidden, laid out for reading
// at leisure (a notification disappears after a few seconds). Painted like the Goals window.
#include "summary.hpp"
#include "settings.hpp"
#include "ui.hpp"
#include <dwmapi.h>
#include <uxtheme.h>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace companion {
namespace {
using namespace ui;
constexpr int max_skills = 12, max_gained = 15, max_used = 8;

std::wstring compact(double value) {
    if (value >= 1e6) return std::format(L"{:.1f}M", value / 1e6);
    if (value >= 1e4) return std::format(L"{:.1f}k", value / 1e3);
    return grouped(static_cast<long long>(value));
}
std::wstring signed_amount(long long value) {
    return (value >= 0 ? L"+" : L"−") + grouped(value);
}
// One line of content. Rows inside a section share a rounded card.
struct Row {
    enum Kind { heading, line, note } kind{};
    std::wstring left, left_detail, right, right_detail;
    bool left_detail_gain{}; // level-ups are highlighted
    bool right_gain{};       // positive amounts are highlighted
    int height{};            // device-independent pixels
    bool danger{};           // notes shown in the alert color (a death)
};

struct Summary {
    GdiplusSession gdiplus;
    BufferedPaintSession buffered_paint;
    std::wstring subtitle;
    std::vector<Row> rows;
    FontPtr title, body, strong, caption; // not "small": the Windows RPC headers define small as a macro
    int scroll{};          // pixels scrolled down
    int content_height{};  // pixels
};
HWND open_window{};

std::vector<Row> build_rows(const ProgressChange& change, bool new_save) {
    std::vector<Row> rows;
    auto heading = [&](const wchar_t* text) { rows.push_back({Row::heading, text, {}, {}, {}, false, false, 34}); };
    auto note = [&](std::wstring text) { rows.push_back({Row::note, std::move(text), {}, {}, {}, false, false, 40}); };
    if (!new_save && !change.deaths) {
        note(L"Melvor has not saved since you hid it, so new progress is not visible yet.");
        return rows;
    }
    if (change.empty()) {
        note(L"No new XP, items, or GP in the latest save.");
        return rows;
    }
    if (change.deaths > 0) {
        // A death is the most important news: first, in red.
        Row died{Row::note, death_text(change)};
        died.danger = true;
        died.height = 56;
        rows.push_back(died);
    }
    // Rates use the time between the two saves, which is what the gains were measured over.
    double hours = static_cast<double>(change.between_saves.count()) / 3600.0;
    if (!change.skills.empty()) {
        heading(L"Skills");
        for (size_t i = 0; i < change.skills.size() && i < max_skills; ++i) {
            const auto& skill = change.skills[i];
            Row row{Row::line, skill.name};
            if (skill.to > skill.from) {
                row.left_detail = std::format(L"Level {} → {}", skill.from, skill.to);
                row.left_detail_gain = true;
            } else {
                row.left_detail = std::format(L"Level {}", skill.to);
            }
            if (skill.xp_to_next > 0)
                row.left_detail += std::format(L" · {} XP to {}", grouped(static_cast<long long>(skill.xp_to_next + 0.999)), skill.to + 1);
            row.right = L"+" + grouped(static_cast<long long>(skill.xp)) + L" XP";
            row.right_gain = true;
            if (change.between_saves >= std::chrono::minutes(1)) row.right_detail = compact(skill.xp / hours) + L" XP/h";
            row.height = 50;
            rows.push_back(row);
        }
        if (change.skills.size() > max_skills) note(std::format(L"+{} more skills", change.skills.size() - max_skills));
    }
    auto items = [&](const wchar_t* title, const std::vector<ItemChange>& list, size_t limit, bool gain) {
        if (list.empty()) return;
        heading(title);
        for (size_t i = 0; i < list.size() && i < limit; ++i) {
            Row row{Row::line, list[i].name, {}, signed_amount(list[i].change)};
            row.right_gain = gain;
            row.height = 32;
            rows.push_back(row);
        }
        if (list.size() > limit) note(std::format(L"+{} more items", list.size() - limit));
    };
    items(L"Items gained", change.gained, max_gained, true);
    items(L"Items used", change.used, max_used, false);
    if (change.gp || change.slayer_coins) {
        heading(L"Currency");
        if (change.gp) rows.push_back({Row::line, L"GP", {}, signed_amount(change.gp), {}, false, change.gp > 0, 32});
        if (change.slayer_coins)
            rows.push_back({Row::line, L"Slayer coins", {}, signed_amount(change.slayer_coins), {}, false, change.slayer_coins > 0, 32});
    }
    return rows;
}

// Fixed areas, in pixels.
int header_height(HWND window) { return dip(window, 78); }
int footer_height(HWND window) { return dip(window, 64); }
RECT content_area(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    return {0, header_height(window), client.right, client.bottom - footer_height(window)};
}
int max_scroll(HWND window, const Summary& summary) {
    auto area = content_area(window);
    return std::max(0, summary.content_height - static_cast<int>(area.bottom - area.top));
}

void paint(HWND window, Summary& summary) {
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(window, &ps);
    RECT client{};
    GetClientRect(window, &client);
    HDC dc{};
    HPAINTBUFFER buffer = BeginBufferedPaint(target, &client, BPBF_TOPDOWNDIB, nullptr, &dc);
    if (!buffer) dc = target;
    auto p = palette();
    auto d = [window](int value) { return dip(window, value); };
    SetDCBrushColor(dc, p.background);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

    draw_text(dc, summary.title.get(), p.text, L"While you were away", {d(20), d(14), client.right - d(20), d(42)}, DT_LEFT);
    draw_text(dc, summary.body.get(), p.muted, summary.subtitle, {d(20), d(42), client.right - d(20), d(64)}, DT_LEFT | DT_END_ELLIPSIS);

    auto area = content_area(window);
    int saved = SaveDC(dc);
    IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
    {
        Gdiplus::Graphics graphics(dc);
        prepare(graphics);
        graphics.SetClip(rectf(area));
        // Cards behind each run of line rows.
        int y = area.top - summary.scroll;
        for (size_t i = 0; i < summary.rows.size();) {
            if (summary.rows[i].kind != Row::line) { y += d(summary.rows[i].height); ++i; continue; }
            int top = y;
            while (i < summary.rows.size() && summary.rows[i].kind == Row::line) { y += d(summary.rows[i].height); ++i; }
            fill_rounded(graphics, Gdiplus::RectF(static_cast<float>(d(12)), static_cast<float>(top - d(4)),
                                                  static_cast<float>(client.right - d(24)), static_cast<float>(y - top + d(8))),
                         dipf(window, 8), p.card);
            y += d(8);
        }
        if (int limit = max_scroll(window, summary); limit > 0) {
            // Scroll position indicator.
            float track = static_cast<float>(area.bottom - area.top) - dipf(window, 8);
            float thumb = std::max(dipf(window, 24), track * track / (track + static_cast<float>(limit)));
            float offset = (track - thumb) * static_cast<float>(summary.scroll) / static_cast<float>(limit);
            fill_rounded(graphics, Gdiplus::RectF(static_cast<float>(client.right) - dipf(window, 7),
                                                  static_cast<float>(area.top) + dipf(window, 4) + offset, dipf(window, 3), thumb),
                         dipf(window, 1.5f), p.line);
        }
    }
    int y = area.top - summary.scroll;
    for (size_t i = 0; i < summary.rows.size(); ++i) {
        const auto& row = summary.rows[i];
        int height = d(row.height);
        RECT r{d(28), y, client.right - d(28), y + height};
        if (row.kind == Row::heading) {
            r.top += d(10);
            draw_text(dc, summary.caption.get(), p.muted, row.left, r, DT_LEFT);
        } else if (row.kind == Row::note) {
            RECT text = r;
            text.left = d(20);
            auto old = SelectObject(dc, summary.body.get());
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, row.danger ? p.alert : p.muted);
            DrawTextW(dc, row.left.c_str(), -1, &text, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, old);
        } else if (row.left_detail.empty() && row.right_detail.empty()) {
            draw_text(dc, summary.body.get(), p.text, row.left, r, DT_LEFT | DT_END_ELLIPSIS);
            draw_text(dc, summary.strong.get(), row.right_gain ? p.gain : p.muted, row.right, r, DT_RIGHT);
        } else {
            RECT upper{r.left, r.top + d(6), r.right, r.top + height / 2 + d(1)};
            RECT lower{r.left, r.top + height / 2 - d(1), r.right, r.bottom - d(6)};
            draw_text(dc, summary.strong.get(), p.text, row.left, upper, DT_LEFT | DT_END_ELLIPSIS);
            draw_text(dc, summary.strong.get(), row.right_gain ? p.gain : p.muted, row.right, upper, DT_RIGHT);
            RECT detail = lower;
            detail.right -= d(96); // leave room for the rate
            draw_text(dc, summary.caption.get(), row.left_detail_gain ? p.gain : p.muted, row.left_detail, detail, DT_LEFT | DT_END_ELLIPSIS);
            draw_text(dc, summary.caption.get(), p.muted, row.right_detail, lower, DT_RIGHT);
        }
        y += height;
        if (i + 1 < summary.rows.size() && row.kind == Row::line && summary.rows[i + 1].kind != Row::line) y += d(8);
    }
    RestoreDC(dc, saved);

    RECT separator{0, client.bottom - footer_height(window), client.right, client.bottom - footer_height(window) + std::max(1, d(1))};
    SetDCBrushColor(dc, p.line);
    FillRect(dc, &separator, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    if (buffer) EndBufferedPaint(buffer, TRUE);
    EndPaint(window, &ps);
}

int measure_content(HWND window, const Summary& summary) {
    int height = 0;
    for (size_t i = 0; i < summary.rows.size(); ++i) {
        height += dip(window, summary.rows[i].height);
        if (summary.rows[i].kind == Row::line && (i + 1 == summary.rows.size() || summary.rows[i + 1].kind != Row::line))
            height += dip(window, 8);
    }
    return height + dip(window, 8);
}

void scroll_to(HWND window, Summary& summary, int value) {
    value = std::clamp(value, 0, max_scroll(window, summary));
    if (value == summary.scroll) return;
    summary.scroll = value;
    InvalidateRect(window, nullptr, FALSE);
}

INT_PTR CALLBACK summary_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* summary = reinterpret_cast<Summary*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lp);
        return FALSE; // sized and shown by show_summary()
    }
    if (!summary) return FALSE;
    switch (message) {
    case WM_ERASEBKGND:
        SetWindowLongPtrW(window, DWLP_MSGRESULT, 1);
        return TRUE;
    case WM_PAINT:
        paint(window, *summary);
        return TRUE;
    case WM_DRAWITEM:
        draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lp), ButtonStyle::outline, summary->body.get(), summary->strong.get());
        return TRUE;
    case WM_MOUSEWHEEL:
        scroll_to(window, *summary, summary->scroll - GET_WHEEL_DELTA_WPARAM(wp) * dip(window, 48) / WHEEL_DELTA);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) DestroyWindow(window); // Close, Enter, or Esc
        return TRUE;
    case WM_CLOSE:
        DestroyWindow(window);
        return TRUE;
    case WM_NCDESTROY:
        // The window owns its state; release it with the window.
        delete summary;
        SetWindowLongPtrW(window, DWLP_USER, 0);
        if (open_window == window) open_window = nullptr;
        return FALSE;
    }
    return FALSE;
}
} // namespace

void show_summary(const ProgressChange& change, std::chrono::seconds hidden_for, bool new_save, HICON icon) {
    if (open_window) DestroyWindow(open_window);
    auto state = std::make_unique<Summary>();
    state->rows = build_rows(change, new_save);
    state->subtitle = L"Hidden " + duration_text(hidden_for);
    if (new_save && change.total_level_to > change.total_level_from)
        state->subtitle += std::format(L" · Total level {} → {}", change.total_level_from, change.total_level_to);
    HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(205), nullptr, summary_proc,
                                     reinterpret_cast<LPARAM>(state.get()));
    if (!window) throw std::runtime_error("Could not open the summary");
    auto* summary = state.release(); // owned by the window from here; freed in WM_NCDESTROY
    open_window = window;

    BOOL dark = dark_mode();
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    if (icon) {
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    }
    summary->title = make_font(window, 13, FW_SEMIBOLD);
    summary->body = make_font(window, 10, FW_NORMAL);
    summary->strong = make_font(window, 10, FW_SEMIBOLD);
    summary->caption = make_font(window, 9, FW_NORMAL);
    summary->content_height = measure_content(window, *summary);

    // Size to the content, up to 70% of the screen height, near the notification area.
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT& work = monitor.rcWork;
    int height = std::min<int>(header_height(window) + summary->content_height + footer_height(window),
                          (work.bottom - work.top) * 7 / 10);
    RECT frame{0, 0, dip(window, 380), height};
    AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window));
    int width = frame.right - frame.left, outer = frame.bottom - frame.top;
    SetWindowPos(window, HWND_TOP, work.right - width - dip(window, 12), work.bottom - outer - dip(window, 12), width, outer,
                 SWP_NOACTIVATE);
    RECT client{};
    GetClientRect(window, &client);
    SetWindowPos(GetDlgItem(window, IDCANCEL), nullptr, client.right - dip(window, 100), client.bottom - dip(window, 48),
                 dip(window, 88), dip(window, 32), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(GetDlgItem(window, IDCANCEL));
}

bool summary_message(MSG& message) {
    return open_window && IsDialogMessageW(open_window, &message);
}

HWND summary_window() { return open_window; }
}
