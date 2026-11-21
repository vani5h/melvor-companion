// The Goals window: a themed checklist.
//
// Native controls keep the behavior people and assistive technology expect. The ListView
// provides selection, scrolling, keyboard navigation, and the checked state (Space toggles it);
// the EDIT control provides typing, clipboard, undo, and IME input. Everything visible is painted
// here instead, because the stock controls only partly support dark mode. Shapes use GDI+ for
// anti-aliased rounded corners; text uses classic GDI so it renders like the rest of Windows.
#include "settings.hpp"
#include "ui.hpp"
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace companion {
namespace {
// Control IDs, shared with src/icons.rc and the desktop test.
enum : int { id_list = 501, id_status = 502, id_input = 503, id_add = 504, id_summary = 506, id_clear = 508 };
constexpr UINT_PTR list_subclass_id = 1;
using namespace ui; // palette, dip, fonts, and drawing helpers shared with the summary window

struct GoalsEditor {
    const std::wstring& text;
    std::function<void(const std::wstring&)> save;
    std::optional<RECT> bounds;
    std::function<void(const RECT&)> save_bounds;
    FontPtr body, strike, heading;
    int line_height{};
    int hover = -1;        // row under the mouse, or -1
    bool tracking_leave{}; // TrackMouseEvent is armed for WM_MOUSELEAVE
    bool allow_edit{};     // the next label edit was started by F2 or a double-click
    bool dragging{};       // a goal is being dragged to a new position
    int drag_from = -1;
    int drop_index = -1;   // insertion point: the dragged goal lands before this row
};

// ---- List helpers ----------------------------------------------------------------------------

std::wstring item_text(HWND list, int index) {
    // LVM_GETITEMTEXT copies at most the buffer size, so grow until the text fits.
    std::wstring text(256, L'\0');
    for (;;) {
        LVITEMW item{};
        item.pszText = text.data();
        item.cchTextMax = static_cast<int>(text.size());
        auto length = static_cast<size_t>(SendMessageW(list, LVM_GETITEMTEXTW, index, reinterpret_cast<LPARAM>(&item)));
        if (length + 1 < text.size()) { text.resize(length); return text; }
        text.resize(text.size() * 2);
    }
}
std::pair<int, int> progress(HWND list) {
    int total = ListView_GetItemCount(list), done = 0;
    for (int i = 0; i < total; ++i) if (ListView_GetCheckState(list, i)) ++done;
    return {done, total};
}
int insert_row(HWND list, int position, std::wstring text, bool done) {
    LVITEMW row{};
    row.mask = LVIF_TEXT;
    row.iItem = position;
    row.pszText = text.data();
    int index = ListView_InsertItem(list, &row);
    if (index >= 0) ListView_SetCheckState(list, index, done);
    return index;
}
void add_row(HWND list, std::wstring text, bool done) { insert_row(list, ListView_GetItemCount(list), std::move(text), done); }

std::wstring trimmed(std::wstring_view text) {
    auto first = text.find_first_not_of(L" \t");
    if (first == std::wstring_view::npos) return {};
    return std::wstring(text.substr(first, text.find_last_not_of(L" \t") - first + 1));
}

// Moves a goal so it lands before row `insertion` (as counted before the move).
void move_row(HWND list, int from, int insertion) {
    if (from < 0 || insertion == from || insertion == from + 1) return;
    auto text = item_text(list, from);
    bool done = ListView_GetCheckState(list, from) != 0;
    ListView_DeleteItem(list, from);
    int target = insertion > from ? insertion - 1 : insertion;
    int index = insert_row(list, target, text, done);
    ListView_SetItemState(list, index, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
    ListView_EnsureVisible(list, index, FALSE);
}

// ---- Layout ----------------------------------------------------------------------------------

struct Layout { RECT summary, percent, bar, card, list, frame, input, add, clear, status, done; int separator; };

Layout compute_layout(HWND dlg, int line_height) {
    RECT client{};
    GetClientRect(dlg, &client);
    auto d = [dlg](int value) { return dip(dlg, value); };
    int w = client.right, h = client.bottom;
    Layout l{};
    l.summary = {d(20), d(12), w - d(80), d(38)};
    l.percent = {w - d(80), d(12), w - d(20), d(38)};
    l.bar = {d(20), d(42), w - d(20), d(48)};
    l.separator = h - d(62);
    l.clear = {d(12), h - d(48), d(150), h - d(16)};
    l.done = {w - d(92), h - d(48), w - d(12), h - d(16)};
    l.status = {d(156), h - d(48), w - d(100), h - d(16)};
    l.frame = {d(12), l.separator - d(52), w - d(86), l.separator - d(16)};
    l.add = {w - d(78), l.frame.top, w - d(12), l.frame.bottom};
    int middle = (l.frame.top + l.frame.bottom) / 2;
    l.input = {l.frame.left + d(12), middle - line_height / 2, l.frame.right - d(12), middle - line_height / 2 + line_height};
    l.card = {d(12), d(62), w - d(12), l.frame.top - d(12)};
    l.list = {l.card.left + d(6), l.card.top + d(6), l.card.right - d(6), l.card.bottom - d(6)};
    return l;
}

void place(HWND dlg, int id, const RECT& r) {
    SetWindowPos(GetDlgItem(dlg, id), nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void apply_layout(HWND dlg, const GoalsEditor& editor) {
    auto l = compute_layout(dlg, editor.line_height);
    place(dlg, id_summary, l.summary);
    place(dlg, id_list, l.list);
    place(dlg, id_input, l.input);
    place(dlg, id_add, l.add);
    place(dlg, id_clear, l.clear);
    place(dlg, id_status, l.status);
    place(dlg, IDOK, l.done);
    // The last column fills the remaining width, so long text never adds a horizontal scrollbar.
    ListView_SetColumnWidth(GetDlgItem(dlg, id_list), 0, LVSCW_AUTOSIZE_USEHEADER);
    InvalidateRect(dlg, nullptr, FALSE);
}

void refresh(HWND dlg) {
    HWND list = GetDlgItem(dlg, id_list);
    auto [done, total] = progress(list);
    SetDlgItemTextW(dlg, id_summary, total ? std::format(L"{} of {} complete", done, total).c_str() : L"No goals yet");
    HWND input = GetDlgItem(dlg, id_input);
    std::wstring typed(GetWindowTextLengthW(input) + 1, L'\0');
    typed.resize(GetWindowTextW(input, typed.data(), static_cast<int>(typed.size())));
    EnableWindow(GetDlgItem(dlg, id_add), typed.find_first_not_of(L" \t") != std::wstring::npos);
    EnableWindow(GetDlgItem(dlg, id_clear), done > 0);
    ListView_SetColumnWidth(list, 0, LVSCW_AUTOSIZE_USEHEADER);
    InvalidateRect(dlg, nullptr, FALSE); // header percentage and progress bar
}

// ---- Painting --------------------------------------------------------------------------------

void paint_dialog(HWND dlg, const GoalsEditor& editor) {
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(dlg, &ps);
    RECT client{};
    GetClientRect(dlg, &client);
    // Draw into an off-screen buffer and copy it once, so resizing does not flicker.
    HDC dc{};
    HPAINTBUFFER buffer = BeginBufferedPaint(target, &client, BPBF_TOPDOWNDIB, nullptr, &dc);
    if (!buffer) dc = target;
    auto p = palette();
    auto l = compute_layout(dlg, editor.line_height);
    auto [done, total] = progress(GetDlgItem(dlg, id_list));
    SetDCBrushColor(dc, p.background);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    {
        Gdiplus::Graphics graphics(dc);
        prepare(graphics);
        auto bar = rectf(l.bar);
        fill_rounded(graphics, bar, bar.Height / 2, p.track);
        if (done > 0) {
            bar.Width = std::max(bar.Height, bar.Width * done / total);
            fill_rounded(graphics, bar, bar.Height / 2, p.accent);
        }
        fill_rounded(graphics, rectf(l.card), dipf(dlg, 8), p.card);
        fill_rounded(graphics, rectf(l.frame), dipf(dlg, 8), p.card);
        bool typing = GetFocus() == GetDlgItem(dlg, id_input);
        stroke_rounded(graphics, rectf(l.frame), dipf(dlg, 8), typing ? p.accent : p.line, dipf(dlg, 1));
        Gdiplus::SolidBrush line(color(p.line));
        graphics.FillRectangle(&line, 0.0f, static_cast<float>(l.separator), static_cast<float>(client.right), dipf(dlg, 1));
    }
    if (total > 0)
        draw_text(dc, editor.body.get(), p.muted, std::format(L"{}%", 100 * done / total), l.percent, DT_RIGHT);
    if (buffer) EndBufferedPaint(buffer, TRUE);
    EndPaint(dlg, &ps);
}

void paint_row(HWND list, int index, HDC dc, const GoalsEditor& editor) {
    auto p = palette();
    RECT row{};
    ListView_GetItemRect(list, index, &row, LVIR_BOUNDS);
    RECT client{};
    GetClientRect(list, &client);
    row.right = client.right;
    bool done = ListView_GetCheckState(list, index) != 0;
    UINT state = ListView_GetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED);
    bool focused_list = GetFocus() == list;
    bool hovered = editor.hover == index;
    bool focus_ring = focused_list && (state & LVIS_FOCUSED)
                      && !(SendMessageW(list, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    SetDCBrushColor(dc, p.card);
    FillRect(dc, &row, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    RECT inner = row;
    InflateRect(&inner, 0, -dip(list, 1));
    float radius = dipf(list, 6);
    {
        Gdiplus::Graphics graphics(dc);
        prepare(graphics);
        if (focused_list && (state & LVIS_SELECTED)) fill_rounded(graphics, rectf(inner), radius, p.selected);
        else if (hovered) fill_rounded(graphics, rectf(inner), radius, p.hover);
        if (focus_ring) stroke_rounded(graphics, rectf(inner), radius, p.accent, dipf(list, 1.5f));
        // Checkbox: the ListView's state-image area is the click target; this is only its look.
        float size = dipf(list, 18);
        float x = static_cast<float>(row.left) + dipf(list, 12);
        float y = static_cast<float>(row.top + row.bottom) / 2 - size / 2;
        Gdiplus::RectF box(x, y, size, size);
        if (done) {
            fill_rounded(graphics, box, dipf(list, 5), p.accent);
            Gdiplus::Pen tick(color(p.on_accent), dipf(list, 2));
            tick.SetStartCap(Gdiplus::LineCapRound);
            tick.SetEndCap(Gdiplus::LineCapRound);
            tick.SetLineJoin(Gdiplus::LineJoinRound);
            Gdiplus::PointF points[]{{x + dipf(list, 4.5f), y + dipf(list, 9.5f)},
                                     {x + dipf(list, 7.8f), y + dipf(list, 12.6f)},
                                     {x + dipf(list, 13.5f), y + dipf(list, 5.8f)}};
            graphics.DrawLines(&tick, points, 3);
        } else {
            stroke_rounded(graphics, box, dipf(list, 5), p.box, dipf(list, 1.5f));
        }
        if (hovered) {
            // Delete mark, matching the click zone in delete_zone().
            float cx = static_cast<float>(row.right) - dipf(list, 20);
            float cy = static_cast<float>(row.top + row.bottom) / 2;
            float half = dipf(list, 4);
            Gdiplus::Pen cross(color(p.muted), dipf(list, 1.5f));
            cross.SetStartCap(Gdiplus::LineCapRound);
            cross.SetEndCap(Gdiplus::LineCapRound);
            graphics.DrawLine(&cross, cx - half, cy - half, cx + half, cy + half);
            graphics.DrawLine(&cross, cx + half, cy - half, cx - half, cy + half);
        }
    }
    RECT text = row;
    text.left += dip(list, 44);
    text.right -= dip(list, hovered ? 40 : 12);
    draw_text(dc, done ? editor.strike.get() : editor.body.get(), done ? p.muted : p.text,
              item_text(list, index), text, DT_LEFT | DT_END_ELLIPSIS);
}

LRESULT draw_list(NMLVCUSTOMDRAW& draw, const GoalsEditor& editor) {
    HWND list = draw.nmcd.hdr.hwndFrom;
    switch (draw.nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPREPAINT:
        paint_row(list, static_cast<int>(draw.nmcd.dwItemSpec), draw.nmcd.hdc, editor);
        return CDRF_SKIPDEFAULT; // the row is fully painted; skip the stock drawing
    case CDDS_POSTPAINT: {
        // The themed ListView marks the column edge in the empty space below the rows; cover it.
        RECT client{};
        GetClientRect(list, &client);
        int count = ListView_GetItemCount(list);
        RECT empty = client;
        if (count > 0) {
            RECT last{};
            ListView_GetItemRect(list, count - 1, &last, LVIR_BOUNDS);
            empty.top = last.bottom;
        }
        if (empty.top < empty.bottom) {
            SetDCBrushColor(draw.nmcd.hdc, palette().card);
            FillRect(draw.nmcd.hdc, &empty, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
        if (count == 0)
            draw_text(draw.nmcd.hdc, editor.body.get(), palette().muted, L"No goals yet. Add one below.", client, DT_CENTER);
        if (editor.dragging && editor.drop_index >= 0 && count > 0) {
            // Insertion line where the dragged goal will land.
            RECT anchor{};
            bool after_last = editor.drop_index >= count;
            ListView_GetItemRect(list, after_last ? count - 1 : editor.drop_index, &anchor, LVIR_BOUNDS);
            float y = static_cast<float>(after_last ? anchor.bottom : anchor.top);
            float thickness = dipf(list, 2);
            Gdiplus::Graphics graphics(draw.nmcd.hdc);
            prepare(graphics);
            fill_rounded(graphics, Gdiplus::RectF(static_cast<float>(client.left) + dipf(list, 8), y - thickness / 2,
                                                  static_cast<float>(client.right - client.left) - dipf(list, 16), thickness),
                         thickness / 2, palette().accent);
        }
        return CDRF_DODEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

void draw_button(const DRAWITEMSTRUCT& item, const GoalsEditor& editor) {
    auto style = item.CtlID == id_add ? ButtonStyle::primary : item.CtlID == IDOK ? ButtonStyle::outline : ButtonStyle::quiet;
    ui::draw_button(item, style, editor.body.get(), editor.heading.get());
}

// ---- Editing ---------------------------------------------------------------------------------

void set_hover(HWND list, GoalsEditor& editor, int index) {
    if (editor.hover == index) return;
    if (editor.hover >= 0) ListView_RedrawItems(list, editor.hover, editor.hover);
    editor.hover = index;
    if (index >= 0) ListView_RedrawItems(list, index, index);
}

int row_at(HWND list, POINT point) {
    LVHITTESTINFO hit{};
    hit.pt = point;
    int index = ListView_HitTest(list, &hit);
    return (hit.flags & LVHT_ONITEM) ? index : -1;
}

bool delete_zone(HWND list, POINT point) {
    RECT client{};
    GetClientRect(list, &client);
    return point.x >= client.right - dip(list, 36);
}

void rehover(HWND list, GoalsEditor& editor) {
    // Rows shift after a deletion; recompute which one is under the cursor.
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(list, &cursor);
    editor.hover = -1;
    RECT client{};
    GetClientRect(list, &client);
    if (PtInRect(&client, cursor)) set_hover(list, editor, row_at(list, cursor));
}

void delete_goal(HWND dlg, GoalsEditor& editor, int index) {
    HWND list = GetDlgItem(dlg, id_list);
    ListView_DeleteItem(list, index);
    int count = ListView_GetItemCount(list);
    if (count > 0) {
        int next = std::min(index, count - 1);
        ListView_SetItemState(list, next, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
    }
    rehover(list, editor);
    refresh(dlg);
}

void add_goal(HWND dlg) {
    HWND input = GetDlgItem(dlg, id_input);
    std::wstring text(GetWindowTextLengthW(input) + 1, L'\0');
    text.resize(GetWindowTextW(input, text.data(), static_cast<int>(text.size())));
    auto first = text.find_first_not_of(L" \t");
    if (first != std::wstring::npos) {
        text = text.substr(first, text.find_last_not_of(L" \t") - first + 1);
        HWND list = GetDlgItem(dlg, id_list);
        add_row(list, text, false);
        ListView_EnsureVisible(list, ListView_GetItemCount(list) - 1, FALSE);
        SetWindowTextW(input, L"");
    }
    refresh(dlg);
    SetFocus(input);
}

void clear_completed(HWND dlg, GoalsEditor& editor) {
    HWND list = GetDlgItem(dlg, id_list);
    for (int i = ListView_GetItemCount(list) - 1; i >= 0; --i)
        if (ListView_GetCheckState(list, i)) ListView_DeleteItem(list, i);
    rehover(list, editor);
    refresh(dlg);
}

int drop_index_at(HWND list, POINT point) {
    int count = ListView_GetItemCount(list);
    for (int i = 0; i < count; ++i) {
        RECT row{};
        ListView_GetItemRect(list, i, &row, LVIR_BOUNDS);
        if (point.y < (row.top + row.bottom) / 2) return i;
    }
    return count;
}

void end_drag(HWND list, GoalsEditor& editor, bool drop) {
    if (!editor.dragging) return;
    editor.dragging = false; // first, so the WM_CAPTURECHANGED from ReleaseCapture is ignored
    if (GetCapture() == list) ReleaseCapture();
    if (drop) move_row(list, editor.drag_from, editor.drop_index);
    editor.drag_from = editor.drop_index = -1;
    InvalidateRect(list, nullptr, FALSE);
}

void start_edit(HWND list, GoalsEditor& editor, int index) {
    if (index < 0) return;
    editor.allow_edit = true;
    ListView_EditLabel(list, index);
    editor.allow_edit = false;
}

bool save_and_close(HWND dlg, GoalsEditor& editor) {
    if (editor.save_bounds) {
        RECT bounds{};
        // The window position is a convenience; failing to store it must not block saving goals.
        try { if (GetWindowRect(dlg, &bounds)) editor.save_bounds(bounds); } catch (...) {}
    }
    try {
        HWND list = GetDlgItem(dlg, id_list);
        std::wstring text;
        for (int i = 0; i < ListView_GetItemCount(list); ++i) {
            text += ListView_GetCheckState(list, i) ? L"[x] " : L"[ ] ";
            text += item_text(list, i);
            text += L"\r\n";
        }
        editor.save(text);
        EndDialog(dlg, IDOK);
        return true;
    } catch (...) {
        SetDlgItemTextW(dlg, id_status, L"Save failed. Your goals are still here.");
        return false;
    }
}

// Subclassing intercepts the ListView's own messages to add hover tracking and the delete mark.
LRESULT CALLBACK list_proc(HWND list, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& editor = *reinterpret_cast<GoalsEditor*>(data);
    switch (message) {
    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (editor.dragging) {
            // Scroll when dragging past the top or bottom edge, then track the insertion point.
            RECT client{};
            GetClientRect(list, &client);
            if (point.y < 0) ListView_Scroll(list, 0, -dip(list, 40));
            else if (point.y > client.bottom) ListView_Scroll(list, 0, dip(list, 40));
            int index = drop_index_at(list, point);
            if (index != editor.drop_index) {
                editor.drop_index = index;
                InvalidateRect(list, nullptr, FALSE);
            }
            return 0;
        }
        set_hover(list, editor, row_at(list, point));
        if (!editor.tracking_leave) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, list, 0};
            editor.tracking_leave = TrackMouseEvent(&track) != FALSE;
        }
        break;
    }
    case WM_MOUSELEAVE:
        editor.tracking_leave = false;
        set_hover(list, editor, -1);
        break;
    case WM_LBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int index = row_at(list, point);
        if (index >= 0 && index == editor.hover && delete_zone(list, point)) {
            delete_goal(GetParent(list), editor, index);
            return 0; // handled; do not let the ListView select the removed row
        }
        break;
    }
    case WM_LBUTTONUP:
        if (editor.dragging) { end_drag(list, editor, true); return 0; }
        break;
    case WM_CAPTURECHANGED:
        end_drag(list, editor, false); // another window took the mouse: cancel the drag
        break;
    case WM_LBUTTONDBLCLK: {
        // Double-click the text to rename a goal. On the checkbox it keeps the stock toggle.
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int index = row_at(list, point);
        if (index >= 0 && point.x >= dip(list, 44) && !delete_zone(list, point)) {
            start_edit(list, editor, index);
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
        // Ctrl+Up / Ctrl+Down move the focused goal: the keyboard equivalent of dragging.
        if ((wp == VK_UP || wp == VK_DOWN) && GetKeyState(VK_CONTROL) < 0) {
            int index = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
            int count = ListView_GetItemCount(list);
            if (index >= 0) {
                if (wp == VK_UP && index > 0) move_row(list, index, index - 1);
                if (wp == VK_DOWN && index + 1 < count) move_row(list, index, index + 2);
            }
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT: {
        // The rename box is the ListView's child, so its colors are requested here.
        auto p = palette();
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, p.text);
        SetBkColor(dc, p.selected);
        SetDCBrushColor(dc, p.selected);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(list, list_proc, list_subclass_id);
        break;
    }
    return DefSubclassProc(list, message, wp, lp);
}

void init_dialog(HWND dlg, GoalsEditor& editor) {
    BOOL dark = dark_mode();
    DwmSetWindowAttribute(dlg, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    auto p = palette();
    editor.body = make_font(dlg, 10, FW_NORMAL, false);
    editor.strike = make_font(dlg, 10, FW_NORMAL, true);
    editor.heading = make_font(dlg, 10, FW_SEMIBOLD, false);
    {
        HDC dc = GetDC(dlg);
        auto old = SelectObject(dc, editor.body.get());
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, old);
        ReleaseDC(dlg, dc);
        editor.line_height = metrics.tmHeight + dip(dlg, 2);
    }
    SendDlgItemMessageW(dlg, id_summary, WM_SETFONT, reinterpret_cast<WPARAM>(editor.heading.get()), FALSE);
    SendDlgItemMessageW(dlg, id_status, WM_SETFONT, reinterpret_cast<WPARAM>(editor.body.get()), FALSE);
    SendDlgItemMessageW(dlg, id_input, WM_SETFONT, reinterpret_cast<WPARAM>(editor.body.get()), FALSE);

    HWND list = GetDlgItem(dlg, id_list);
    SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(editor.body.get()), FALSE);
    // Theme first: a theme change rebuilds the checkbox images, which would undo the next steps.
    SetWindowTheme(list, dark_mode() ? L"DarkMode_Explorer" : L"Explorer", nullptr); // dark scrollbar
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    // Replace the stock checkbox images with blank ones of our size. They are never drawn, but they
    // set the row height and the width of the click target that toggles a goal.
    HIMAGELIST zone = ImageList_Create(dip(dlg, 44), dip(dlg, 40), ILC_COLOR32, 2, 0);
    ImageList_SetImageCount(zone, 2); // index 1 = unchecked, 2 = checked
    if (HIMAGELIST stock = ListView_SetImageList(list, zone, LVSIL_STATE)) ImageList_Destroy(stock);
    ListView_SetBkColor(list, p.card);
    ListView_SetTextBkColor(list, p.card);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    ListView_InsertColumn(list, 0, &column);
    SetWindowSubclass(list, list_proc, list_subclass_id, reinterpret_cast<DWORD_PTR>(&editor));

    // Saved format: one goal per line, "[x] " or "[ ] " marks completion; older plain lines are open goals.
    const auto& text = editor.text;
    for (size_t start = 0; start < text.size();) {
        auto end = text.find(L'\n', start);
        auto line = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        bool checked = line.starts_with(L"[x] ") || line.starts_with(L"[X] ");
        if (checked || line.starts_with(L"[ ] ")) line.erase(0, 4);
        if (!line.empty()) add_row(list, line, checked);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (ListView_GetItemCount(list) > 0) ListView_SetItemState(list, 0, LVIS_FOCUSED, LVIS_FOCUSED);

    HWND input = GetDlgItem(dlg, id_input);
    SendMessageW(input, EM_SETLIMITTEXT, 1000, 0);
    SendMessageW(input, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Add a goal..."));

    // Reopen where the window was last closed, if that spot is still on a connected monitor.
    // Otherwise size it for its content and center it on the current monitor's work area.
    if (editor.bounds && MonitorFromRect(&*editor.bounds, MONITOR_DEFAULTTONULL)) {
        const RECT& last = *editor.bounds;
        SetWindowPos(dlg, nullptr, last.left, last.top, std::max<int>(last.right - last.left, dip(dlg, 320)),
                     std::max<int>(last.bottom - last.top, dip(dlg, 360)), SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        RECT frame{0, 0, dip(dlg, 360), dip(dlg, 480)};
        AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(dlg, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongPtrW(dlg, GWL_EXSTYLE)), GetDpiForWindow(dlg));
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST), &monitor);
        int width = frame.right - frame.left, height = frame.bottom - frame.top;
        const RECT& work = monitor.rcWork;
        SetWindowPos(dlg, nullptr, work.left + (work.right - work.left - width) / 2,
                     work.top + (work.bottom - work.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    apply_layout(dlg, editor);
    refresh(dlg);
}

INT_PTR CALLBACK goals_proc(HWND dlg, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        init_dialog(dlg, *reinterpret_cast<GoalsEditor*>(lp));
        SetFocus(GetDlgItem(dlg, id_input)); // ready to type a new goal
        return FALSE;                        // FALSE: we set the focus ourselves
    }
    auto* editor = reinterpret_cast<GoalsEditor*>(GetWindowLongPtrW(dlg, DWLP_USER));
    if (!editor) return FALSE; // messages that arrive before WM_INITDIALOG
    auto p = palette();
    switch (message) {
    case WM_SIZE:
        apply_layout(dlg, *editor);
        return TRUE;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
        limits->ptMinTrackSize = {dip(dlg, 320), dip(dlg, 360)};
        return TRUE;
    }
    case WM_ERASEBKGND:
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, 1); // WM_PAINT covers the whole client area
        return TRUE;
    case WM_PAINT:
        paint_dialog(dlg, *editor);
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        COLORREF background = message == WM_CTLCOLOREDIT ? p.card : p.background;
        SetTextColor(dc, p.text);
        SetBkColor(dc, background);
        SetDCBrushColor(dc, background);
        return reinterpret_cast<INT_PTR>(GetStockObject(DC_BRUSH));
    }
    case WM_DRAWITEM:
        draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lp), *editor);
        return TRUE;
    case WM_NOTIFY: {
        auto* notice = reinterpret_cast<NMHDR*>(lp);
        if (notice->idFrom != id_list) return FALSE;
        if (notice->code == NM_CUSTOMDRAW) {
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, draw_list(*reinterpret_cast<NMLVCUSTOMDRAW*>(lp), *editor));
            return TRUE;
        }
        if (notice->code == LVN_ITEMCHANGED && (reinterpret_cast<NMLISTVIEW*>(lp)->uChanged & LVIF_STATE))
            refresh(dlg);
        if (notice->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(lp)->wVKey == VK_DELETE) {
            int index = ListView_GetNextItem(notice->hwndFrom, -1, LVNI_FOCUSED);
            if (index >= 0) delete_goal(dlg, *editor, index);
        }
        if (notice->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(lp)->wVKey == VK_F2)
            editor->allow_edit = true; // the ListView starts the rename itself
        if (notice->code == LVN_BEGINDRAG) {
            editor->dragging = true;
            editor->drag_from = editor->drop_index = reinterpret_cast<NMLISTVIEW*>(lp)->iItem;
            set_hover(notice->hwndFrom, *editor, -1);
            SetCapture(notice->hwndFrom);
        }
        if (notice->code == LVN_BEGINLABELEDITW) {
            // Renames start only from F2 or a double-click, not from clicking a selected row.
            bool allowed = editor->allow_edit;
            editor->allow_edit = false;
            if (allowed) {
                HWND edit = ListView_GetEditControl(notice->hwndFrom);
                SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(editor->body.get()), FALSE);
                SendMessageW(edit, EM_SETLIMITTEXT, 1000, 0);
            }
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, allowed ? FALSE : TRUE); // TRUE cancels the edit
            return TRUE;
        }
        if (notice->code == LVN_ENDLABELEDITW) {
            auto& item = reinterpret_cast<NMLVDISPINFOW*>(lp)->item;
            if (item.pszText) { // null when the rename was cancelled
                auto text = trimmed(item.pszText);
                if (!text.empty()) ListView_SetItemText(notice->hwndFrom, item.iItem, 0, text.data());
            }
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, FALSE); // the trimmed text is already stored
            return TRUE;
        }
        if (notice->code == NM_SETFOCUS || notice->code == NM_KILLFOCUS)
            InvalidateRect(notice->hwndFrom, nullptr, FALSE); // selection and focus ring follow focus
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case id_input:
            if (HIWORD(wp) == EN_CHANGE) refresh(dlg);
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) InvalidateRect(dlg, nullptr, FALSE);
            return TRUE;
        case id_add:
            add_goal(dlg);
            return TRUE;
        case id_clear:
            clear_completed(dlg, *editor);
            return TRUE;
        case IDOK: {
            // While renaming, Enter finishes the rename. Enter in the input adds the goal;
            // Enter elsewhere, or the Done button, closes.
            HWND list = GetDlgItem(dlg, id_list);
            if (ListView_GetEditControl(list)) { SetFocus(list); return TRUE; } // losing focus commits
            HWND input = GetDlgItem(dlg, id_input);
            if (GetFocus() == input && GetWindowTextLengthW(input) > 0) add_goal(dlg);
            else save_and_close(dlg, *editor);
            return TRUE;
        }
        case IDCANCEL: { // Esc or the title-bar close button; closing always saves
            HWND list = GetDlgItem(dlg, id_list);
            if (ListView_GetEditControl(list)) { ListView_CancelEditLabel(list); return TRUE; }
            if (editor->dragging) { end_drag(list, *editor, false); return TRUE; }
            save_and_close(dlg, *editor);
            return TRUE;
        }
        }
        return FALSE;
    }
    return FALSE;
}
} // namespace

void edit_goals(HWND owner, const std::wstring& text, std::function<void(const std::wstring&)> save,
                std::optional<RECT> bounds, std::function<void(const RECT&)> save_bounds) {
    GdiplusSession gdiplus;
    BufferedPaintSession buffered_paint;
    GoalsEditor editor{text, std::move(save), bounds, std::move(save_bounds)};
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(204), owner, goals_proc,
                        reinterpret_cast<LPARAM>(&editor)) == -1)
        throw std::runtime_error("Could not open goals");
}
} // namespace companion
