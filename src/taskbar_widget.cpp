#include "taskbar_widget.hpp"
#include "taskbar_layout.hpp"
#include "log.hpp"
#include "settings.hpp"
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <format>
#include <optional>
#include <string_view>

namespace companion {
namespace {
// The switch art's drawn rows inside the 28 px icon entry.
constexpr int art_width = TaskbarWidget::art_icon_size, art_height = 16, art_top = (art_width - art_height) / 2;
constexpr wchar_t window_class[] = L"MelvorCompanion.TaskbarWidget";
constexpr wchar_t tooltip_property[] = L"MelvorCompanion.WidgetTooltip"; // read by the desktop test

// True while a fullscreen app (a game, a video) is in front on the widget's monitor.
// Explorer keeps the taskbar in the z-order underneath, so the widget can still get clicks
// and hover there; opening the menu then takes the foreground and tabs the player out.
// Not SHQueryUserNotificationState: it reported QUNS_RUNNING_D3D_FULL_SCREEN while the
// taskbar itself was in front, so it would block the widget whenever a game runs behind.
bool fullscreen_app_in_front(HWND widget) {
    // Both exclusive and borderless fullscreen put a window covering the whole monitor in front.
    HWND front = GetForegroundWindow();
    if (!front || IsZoomed(front)) return false; // a maximized window leaves the taskbar usable
    wchar_t name[64]{};
    GetClassNameW(front, name, 64);
    const std::wstring_view kind(name);
    if (kind == L"Progman" || kind == L"WorkerW" || kind == L"Shell_TrayWnd" || kind == L"Shell_SecondaryTrayWnd")
        return false; // the desktop or the taskbar itself
    MONITORINFO monitor{sizeof(monitor)};
    RECT covered{};
    if (!GetMonitorInfoW(MonitorFromWindow(widget, MONITOR_DEFAULTTONEAREST), &monitor) || !GetWindowRect(front, &covered))
        return false;
    return covered.left <= monitor.rcMonitor.left && covered.top <= monitor.rcMonitor.top &&
           covered.right >= monitor.rcMonitor.right && covered.bottom >= monitor.rcMonitor.bottom;
}
}

TaskbarWidget::~TaskbarWidget() { remove(); }

void TaskbarWidget::set_owner(HWND owner, UINT click_message) {
    owner_ = owner;
    click_message_ = click_message;
}

void TaskbarWidget::set_art(HICON awake, HICON asleep, HICON missing, HICON dead) {
    art_[0] = awake;
    art_[1] = asleep;
    art_[2] = missing;
    art_[3] = dead;
    drawn_ = false; // redraw with the new art
}

void TaskbarWidget::remove() {
    if (IsWindow(window_)) DestroyWindow(window_);
    if (IsWindow(tooltip_)) DestroyWindow(tooltip_);
    window_ = tooltip_ = nullptr;
    if (owner_) RemovePropW(owner_, tooltip_property);
    drawn_ = false;
}

void TaskbarWidget::theme_changed() {
    if (IsWindow(tooltip_)) SetWindowTheme(tooltip_, dark_mode() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    drawn_ = false;
}

void TaskbarWidget::track_hover() {
    if (!IsWindowVisible(window_)) return;
    POINT cursor{};
    RECT bounds{};
    if (!GetCursorPos(&cursor) || !GetWindowRect(window_, &bounds)) return;
    // Only when the widget itself is under the cursor, not a window covering it.
    if (PtInRect(&bounds, cursor) && WindowFromPoint(cursor) == window_ && !fullscreen_app_in_front(window_) &&
        !IsWindowVisible(tooltip_)) PostMessageW(window_, WM_MOUSEHOVER, 0, 0);
    else if (!PtInRect(&bounds, cursor) && IsWindowVisible(tooltip_)) PostMessageW(window_, WM_MOUSELEAVE, 0, 0);
}

LRESULT CALLBACK TaskbarWidget::procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<TaskbarWidget*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (self && message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track), TME_HOVER | TME_LEAVE, window, 350};
        TrackMouseEvent(&track);
    }
    if (self && message == WM_MOUSEHOVER && fullscreen_app_in_front(window)) return 0;
    if (self && (message == WM_MOUSEHOVER || message == WM_MOUSELEAVE)) {
        TTTOOLINFOW tool{sizeof(tool)};
        tool.hwnd = self->owner_;
        tool.uId = reinterpret_cast<UINT_PTR>(window);
        if (message == WM_MOUSEHOVER) {
            POINT cursor{};
            GetCursorPos(&cursor);
            SendMessageW(self->tooltip_, TTM_TRACKPOSITION, 0, MAKELPARAM(cursor.x, cursor.y - 40));
        }
        SendMessageW(self->tooltip_, TTM_TRACKACTIVATE, message == WM_MOUSEHOVER, reinterpret_cast<LPARAM>(&tool));
        if (message == WM_MOUSEHOVER) {
            RECT box{}, target{};
            GetWindowRect(self->tooltip_, &box);
            GetWindowRect(window, &target);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            int x = std::max<int>(monitor.rcWork.left, std::min<int>(target.left, monitor.rcWork.right - (box.right - box.left)));
            int y = target.top - (box.bottom - box.top) - 6;
            SendMessageW(self->tooltip_, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
        }
        return 0;
    }
    if (self && (message == WM_LBUTTONUP || message == WM_RBUTTONUP)) {
        if (IsWindow(self->tooltip_)) SendMessageW(self->tooltip_, TTM_POP, 0, 0);
        if (fullscreen_app_in_front(window)) {
            log_event("Ignored a taskbar widget click under a fullscreen app.");
            return 0;
        }
        PostMessageW(self->owner_, self->click_message_, 0, WM_RBUTTONUP);
        return 0;
    }
    if (self && message == WM_NCDESTROY) self->window_ = nullptr;
    return DefWindowProcW(window, message, wp, lp);
}

bool TaskbarWidget::ensure(HWND bar) {
    if (IsWindow(window_) && GetParent(window_) == bar) return true;
    remove();
    WNDCLASSW wc{};
    wc.lpfnWndProc = procedure;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = window_class;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, window_class, L"", WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, 1, 1, bar, nullptr, wc.hInstance, this);
    if (!window_) return false;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, L"Melvor Companion widget",
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, owner_, nullptr, wc.hInstance, nullptr);
    if (tooltip_) {
        SetPropW(owner_, tooltip_property, tooltip_);
        SetWindowTheme(tooltip_, dark_mode() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        TTTOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
        tool.hwnd = owner_;
        tool.uId = reinterpret_cast<UINT_PTR>(window_);
        tool.lpszText = LPSTR_TEXTCALLBACKW; // the owner supplies the text on demand
        if (!SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)))
            log_event("Could not attach taskbar hover tooltip.");
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 420);
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 350);
    }
    return true;
}

// Whole-number scale keeps every art pixel a crisp square: 1x (28x16) at 100% display
// scaling, 2x from 150%, but never larger than the space allows (a horizontal taskbar's
// height, or a vertical taskbar's width less margins).
SIZE TaskbarWidget::art_size(int max_width, int max_height) const {
    int scale = std::max(1, MulDiv(1, static_cast<int>(GetDpiForWindow(window_)), 96));
    scale = std::min({scale, std::max(1, max_height / art_height), std::max(1, max_width / art_width)});
    return {art_width * scale, art_height * scale};
}

// Spacing is written in device-independent pixels (1/96 inch) and scaled here.
int TaskbarWidget::px(int value) const {
    return MulDiv(value, window_ ? static_cast<int>(GetDpiForWindow(window_)) : 96, 96);
}

// The text font: the classic GUI font, scaled to the widget's DPI. The stock DEFAULT_GUI_FONT is
// always sized for 96 DPI, so it would look tiny at 150%. Recreated only when the DPI changes.
HFONT TaskbarWidget::font() {
    UINT dpi = window_ ? GetDpiForWindow(window_) : 96;
    if (font_ && font_dpi_ == dpi) return font_.get();
    LOGFONTW description{};
    GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(description), &description);
    description.lfHeight = MulDiv(description.lfHeight, static_cast<int>(dpi), 96);
    if (HFONT created = CreateFontIndirectW(&description)) {
        font_.reset(created);
        font_dpi_ = dpi;
    }
    return font_ ? font_.get() : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

void TaskbarWidget::render(int width, int height, bool with_text) {
    HDC screen = GetDC(nullptr);
    HDC buffer = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void* pixels{};
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!buffer || !bitmap) {
        if (buffer) DeleteDC(buffer);
        if (bitmap) DeleteObject(bitmap);
        ReleaseDC(nullptr, screen);
        return;
    }
    auto previous_bitmap = SelectObject(buffer, bitmap);
    HDC dc = buffer;
    RECT area{0, 0, width, height};
    SetDCBrushColor(dc, RGB(1, 1, 1));
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SIZE size = art_size(width - px(8), height);
    // With text the switch sits at the left margin; alone it is centered (on a horizontal
    // taskbar that is the same 4 DIP margin; on a vertical one it centers in the column).
    int art_w = size.cx, art_h = size.cy;
    int art_x = with_text ? px(4) : (width - art_w) / 2;
    // Draw the art 1:1 into a small bitmap keyed with the transparent color, then enlarge
    // only its 16 art rows. COLORONCOLOR is nearest-neighbor: at a whole-number scale each
    // pixel becomes an exact block, so nothing is dropped or blurred.
    HICON art = art_[static_cast<int>(shown_.art)];
    HDC art_dc = CreateCompatibleDC(screen);
    HBITMAP art_bitmap = CreateCompatibleBitmap(screen, art_width, art_width);
    if (art_dc && art_bitmap) {
        auto previous_art = SelectObject(art_dc, art_bitmap);
        RECT art_area{0, 0, art_width, art_width};
        SetDCBrushColor(art_dc, RGB(1, 1, 1));
        FillRect(art_dc, &art_area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        DrawIconEx(art_dc, 0, 0, art, art_width, art_width, 0, nullptr, DI_NORMAL);
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchBlt(dc, art_x, (height - art_h) / 2, art_w, art_h, art_dc, 0, art_top, art_width, art_height, SRCCOPY);
        SelectObject(art_dc, previous_art);
    }
    if (art_bitmap) DeleteObject(art_bitmap);
    if (art_dc) DeleteDC(art_dc);
    if (shown_.alert) {
        // Idle highlight: a red box around the whole widget (switch and text), as four solid edges.
        int pad = px(3), line = px(2);
        RECT box{art_x - pad, (height - art_h) / 2 - pad, width - (art_x - pad), (height + art_h) / 2 + pad};
        SetDCBrushColor(dc, idle_red);
        auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        RECT edges[]{{box.left, box.top, box.right, box.top + line}, {box.left, box.bottom - line, box.right, box.bottom},
                     {box.left, box.top, box.left + line, box.bottom}, {box.right - line, box.top, box.right, box.bottom}};
        for (const auto& edge : edges) FillRect(dc, &edge, brush);
    }
    auto old = SelectObject(dc, font());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    area.left = art_x + art_w + px(6);
    area.right -= px(6);
    if (with_text && area.right > area.left)
        DrawTextW(dc, shown_.text.c_str(), -1, &area, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, old);
    GdiFlush();
    auto* frame = static_cast<DWORD*>(pixels);
    for (int i = 0; i < width * height; ++i) {
        DWORD color = frame[i] & 0xffffff;
        // Alpha 1 keeps otherwise clear pixels mouse-active.
        frame[i] = color == 0x010101 || color == 0 ? 0x01000000 : color | 0xff000000;
    }
    SIZE size_pixels{width, height};
    POINT origin{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(window_, screen, nullptr, &size_pixels, buffer, &origin, 0, &blend, ULW_ALPHA))
        log_event("UpdateLayeredWindow failed for taskbar widget.");
    SelectObject(buffer, previous_bitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    ReleaseDC(nullptr, screen);
}

bool TaskbarWidget::update(const Look& look) {
    HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND notification = FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT bounds{}, tray_bounds{};
    if (!bar || !GetWindowRect(bar, &bounds) || !ensure(bar)) {
        if (IsWindow(window_)) ShowWindow(window_, SW_HIDE);
        return false;
    }
    bool known_tray = notification && GetWindowRect(notification, &tray_bounds) && tray_bounds.right > tray_bounds.left;
    // Docked left or right, the taskbar is a column: the widget stacks above the notification
    // area and shows only the switch, since text does not fit across a column.
    bool vertical = bounds.bottom - bounds.top > bounds.right - bounds.left;

    // An auto-hiding taskbar slides mostly off screen. The widget is its child and slides with
    // it, so its last placement stays right; re-placing an off-screen taskbar would only churn.
    // Keep the position and just redraw if the look changed.
    MONITORINFO monitor{sizeof(monitor)};
    RECT on_screen{};
    if (drawn_ && IsWindowVisible(window_) && GetMonitorInfoW(MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST), &monitor)) {
        IntersectRect(&on_screen, &bounds, &monitor.rcMonitor);
        int thickness = vertical ? bounds.right - bounds.left : bounds.bottom - bounds.top;
        int showing = vertical ? on_screen.right - on_screen.left : on_screen.bottom - on_screen.top;
        if (showing < thickness / 2) {
            if (!(look == shown_)) {
                RECT current{};
                GetWindowRect(window_, &current);
                shown_ = look;
                render(current.right - current.left, current.bottom - current.top, !vertical && current_has_text_);
            }
            return true;
        }
    }

    int bar_width = bounds.right - bounds.left, bar_height = bounds.bottom - bounds.top;
    HDC dc = GetDC(window_);
    auto old = SelectObject(dc, font());
    SIZE text{};
    GetTextExtentPoint32W(dc, look.text.c_str(), static_cast<int>(look.text.size()), &text);
    SelectObject(dc, old);
    ReleaseDC(window_, dc);
    SIZE art = vertical ? art_size(bar_width - px(8), bar_height) : art_size(bar_width, bar_height);
    // Horizontal: 4 DIP left margin, art, 6 DIP gap, text, 6 DIP right margin (must match
    // render); the strip is the taskbar's height. Vertical: the switch alone, the column's
    // width, with 6 DIP above and below.
    int icon_width = px(4) + art.cx + px(4);
    int full_width = look.text.empty() ? icon_width : icon_width + px(8) + text.cx;
    int icon_height = px(6) + art.cy + px(6);

    // Find the taskbar neighbors to keep clear of: other widgets (such as a battery widget),
    // the notification area, the task buttons. While Start or Search is open, Windows 11
    // reports them hidden, and may shift them across the bar, although they keep their place
    // along it and keep drawing. Only the extent along the bar matters for placement, so a
    // neighbor seen visible keeps its space until its window is destroyed or moves along the bar.
    struct Scan {
        HWND self; RECT bar; bool vertical; const std::map<HWND, RECT>* known;
        std::map<HWND, RECT> found; std::string names; std::optional<int> buttons_end;
    };
    Scan scan{window_, bounds, vertical, &neighbors_, {}, {}, {}};
    EnumChildWindows(bar, [](HWND child, LPARAM data) -> BOOL {
        auto& state = *reinterpret_cast<Scan*>(data);
        RECT rect{};
        if (child == state.self || !GetWindowRect(child, &rect) || rect.right <= rect.left || rect.bottom <= rect.top)
            return TRUE;
        char name[64]{};
        GetClassNameA(child, name, 64);
        std::string_view cls(name);
        // These are surfaces containing controls. Actual app buttons and independent
        // widgets can also be wider than half the bar and must always reserve space.
        if (cls == "Windows.UI.Composition.DesktopWindowContentBridge" || cls == "Windows.UI.Core.CoreWindow"
            || cls == "ReBarWindow32")
            return TRUE;
        bool buttons = cls == "MSTaskSwWClass" || cls == "MSTaskListWClass";
        int start = state.vertical ? rect.top : rect.left, end = state.vertical ? rect.bottom : rect.right;
        if (IsWindowVisible(child)) {
            RECT overlap{};
            if (!IntersectRect(&overlap, &rect, &state.bar)) return TRUE; // not on the bar
            state.found[child] = rect;
            if (buttons) state.buttons_end = std::max<int>(end, state.buttons_end.value_or(end));
            state.names += std::format(" [{} {}-{}]", name, start, end);
            return TRUE;
        }
        auto known = state.known->find(child);
        if (known != state.known->end()
            && (state.vertical ? known->second.top == rect.top && known->second.bottom == rect.bottom
                               : known->second.left == rect.left && known->second.right == rect.right)) {
            state.found[child] = known->second; // keep the last on-bar rectangle
            if (buttons) state.buttons_end = std::max<int>(end, state.buttons_end.value_or(end));
            state.names += std::format(" [{} {}-{} remembered]", name, start, end);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&scan));
    // Start/Search can give the descendant scan an incomplete view of the bar even though its
    // battery strip is still addressable as a direct child. Read its current bounds on each
    // layout so a resized or removed strip frees space. Its own WS_VISIBLE style is checked:
    // IsWindowVisible also looks at the taskbar's containers, which Windows hides meanwhile.
    if (HWND battery = FindWindowExW(bar, nullptr, L"WirelessBatteryWidget", nullptr)) {
        RECT rect{}, intersection{};
        if ((GetWindowLongPtrW(battery, GWL_STYLE) & WS_VISIBLE) && GetWindowRect(battery, &rect)
            && IntersectRect(&intersection, &bounds, &rect) && scan.found.emplace(battery, rect).second)
            scan.names += std::format(" [WirelessBatteryWidget {}-{} direct]",
                                      vertical ? rect.top : rect.left, vertical ? rect.bottom : rect.right);
    }
    neighbors_ = std::move(scan.found);
    // Where the app buttons really end. On Windows 11 the legacy button window can report a
    // stale edge (175% scaling put the widget on top of the last apps), so also use the
    // drawn buttons measured through UI Automation, whichever reaches further.
    if (auto apps = probe_.app_buttons_end()) {
        int apps_end = vertical ? apps->bottom : apps->right;
        if (!scan.buttons_end || apps_end > *scan.buttons_end) scan.buttons_end = apps_end;
        scan.names += std::format(" [app buttons end {} automation]", apps_end);
    }

    // Place along the bar: x for a horizontal taskbar, y for a vertical one.
    std::optional<WidgetPlacement> fit;
    if (known_tray && scan.buttons_end) {
        if (vertical)
            fit = fit_taskbar_widget_vertical(std::max<int>(bounds.top, *scan.buttons_end),
                                              std::min(tray_bounds.top, bounds.bottom), icon_height, neighbors_, px(8));
        else
            fit = fit_taskbar_widget(std::max<int>(bounds.left, *scan.buttons_end), std::min(tray_bounds.left, bounds.right),
                                     full_width, icon_width, neighbors_, px(8));
    }
    int x = vertical ? bounds.left : fit ? fit->x : bounds.left;
    int y = vertical ? (fit ? fit->x : bounds.top) : bounds.top;
    int width = !fit ? 0 : vertical ? bar_width : fit->width;
    int height = !fit ? 0 : vertical ? fit->width : bar_height;
    bool with_text = fit && !vertical && fit->width == full_width && !look.text.empty();

    // Diagnostics: record each distinct placement decision (rare; only when inputs change).
    auto inputs = std::format("Widget placement: bar {}-{}{}, tray {}-{}{}", vertical ? bounds.top : bounds.left,
                              vertical ? bounds.bottom : bounds.right, vertical ? " vertical" : "",
                              vertical ? tray_bounds.top : tray_bounds.left, vertical ? tray_bounds.bottom : tray_bounds.right,
                              notification ? "" : " (estimated)");
    int along = vertical ? y : x;
    if (inputs != last_inputs_ || scan.names != last_names_ || along != last_x_) {
        probe_.request(); // the taskbar changed: measure the app buttons again
        log_event(std::format("{}, {} {}, {} {}{}", inputs, vertical ? "height" : "width", vertical ? height : width,
                              vertical ? "y" : "x", along, scan.names));
        last_inputs_ = inputs;
        last_names_ = scan.names;
        last_x_ = along;
    }
    if (!fit) {
        if (IsWindowVisible(tooltip_)) SendMessageW(window_, WM_MOUSELEAVE, 0, 0);
        if (GetWindowLongPtrW(window_, GWL_STYLE) & WS_VISIBLE) ShowWindow(window_, SW_HIDE);
        return false;
    }
    bool changed = !drawn_ || !(look == shown_) || with_text != current_has_text_;
    shown_ = look;
    drawn_ = true;
    current_has_text_ = with_text;
    SetWindowTextW(window_, look.text.c_str());
    RECT current{};
    GetWindowRect(window_, &current);
    bool resized = current.right - current.left != width || current.bottom - current.top != height;
    if (changed || resized) render(width, height, with_text);
    POINT local{x, y};
    ScreenToClient(bar, &local);
    if (!IsWindowVisible(window_) || current.left != x || current.top != y || resized || GetWindow(window_, GW_HWNDPREV))
        SetWindowPos(window_, HWND_TOP, local.x, local.y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}
}
