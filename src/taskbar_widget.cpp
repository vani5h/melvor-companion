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
// drawn rows inside the 28 px entry
constexpr int art_width = TaskbarWidget::art_icon_size, art_height = 16, art_top = (art_width - art_height) / 2;
constexpr wchar_t window_class[] = L"MelvorCompanion.TaskbarWidget";
constexpr wchar_t tooltip_property[] = L"MelvorCompanion.WidgetTooltip";

// Fullscreen app in front: the taskbar still gets clicks underneath, and opening the menu tabs the player out.
// Not SHQueryUserNotificationState: it reports D3D fullscreen while a game is only running behind.
bool fullscreen_app_in_front(HWND widget) {
    HWND front = GetForegroundWindow();
    if (!front || IsZoomed(front)) return false; // maximized leaves the taskbar usable
    wchar_t name[64]{};
    GetClassNameW(front, name, 64);
    const std::wstring_view kind(name);
    if (kind == L"Progman" || kind == L"WorkerW" || kind == L"Shell_TrayWnd" || kind == L"Shell_SecondaryTrayWnd")
        return false;
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
    drawn_ = false;
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
    // only if nothing covers the widget
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
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        if (!SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)))
            log_event("Could not attach taskbar hover tooltip.");
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 420);
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 350);
    }
    return true;
}

// whole-number scale only (1x at 100%, 2x from 150%), limited by the space
SIZE TaskbarWidget::art_size(int max_width, int max_height) const {
    int scale = std::max(1, MulDiv(1, static_cast<int>(GetDpiForWindow(window_)), 96));
    scale = std::min({scale, std::max(1, max_height / art_height), std::max(1, max_width / art_width)});
    return {art_width * scale, art_height * scale};
}

int TaskbarWidget::px(int value) const {
    return MulDiv(value, window_ ? static_cast<int>(GetDpiForWindow(window_)) : 96, 96);
}

// DEFAULT_GUI_FONT stays at 96 DPI, so make one for this DPI
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
    int art_w = size.cx, art_h = size.cy;
    int art_x = with_text ? px(4) : (width - art_w) / 2;
    // draw 1:1 then stretch nearest-neighbor (COLORONCOLOR) so pixels stay square
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
        // idle highlight
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
        // alpha 1 keeps clear pixels clickable
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
    // vertical taskbar: switch only, above the tray
    bool vertical = bounds.bottom - bounds.top > bounds.right - bounds.left;

    // auto-hide: the widget slides with its parent, so keep the placement and just redraw
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
    // must match render()
    int icon_width = px(4) + art.cx + px(4);
    int full_width = look.text.empty() ? icon_width : icon_width + px(8) + text.cx;
    int icon_height = px(6) + art.cy + px(6);

    // Neighbors to avoid. While Start/Search is open Win11 reports them hidden, so keep any seen visible until it is destroyed or moves.
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
        // wide app buttons and widgets still reserve space
        if (cls == "Windows.UI.Composition.DesktopWindowContentBridge" || cls == "Windows.UI.Core.CoreWindow"
            || cls == "ReBarWindow32")
            return TRUE;
        bool buttons = cls == "MSTaskSwWClass" || cls == "MSTaskListWClass";
        int start = state.vertical ? rect.top : rect.left, end = state.vertical ? rect.bottom : rect.right;
        if (IsWindowVisible(child)) {
            RECT overlap{};
            if (!IntersectRect(&overlap, &rect, &state.bar)) return TRUE;
            state.found[child] = rect;
            if (buttons) state.buttons_end = std::max<int>(end, state.buttons_end.value_or(end));
            state.names += std::format(" [{} {}-{}]", name, start, end);
            return TRUE;
        }
        auto known = state.known->find(child);
        if (known != state.known->end()
            && (state.vertical ? known->second.top == rect.top && known->second.bottom == rect.bottom
                               : known->second.left == rect.left && known->second.right == rect.right)) {
            state.found[child] = known->second; // keep the last on-bar rect
            if (buttons) state.buttons_end = std::max<int>(end, state.buttons_end.value_or(end));
            state.names += std::format(" [{} {}-{} remembered]", name, start, end);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&scan));
    // Start/Search can hide the battery widget from the scan; read it directly. Check its own WS_VISIBLE, not IsWindowVisible.
    if (HWND battery = FindWindowExW(bar, nullptr, L"WirelessBatteryWidget", nullptr)) {
        RECT rect{}, intersection{};
        if ((GetWindowLongPtrW(battery, GWL_STYLE) & WS_VISIBLE) && GetWindowRect(battery, &rect)
            && IntersectRect(&intersection, &bounds, &rect) && scan.found.emplace(battery, rect).second)
            scan.names += std::format(" [WirelessBatteryWidget {}-{} direct]",
                                      vertical ? rect.top : rect.left, vertical ? rect.bottom : rect.right);
    }
    neighbors_ = std::move(scan.found);
    // Win11's legacy button window can report a stale edge, so also use the UIA-measured buttons.
    if (auto apps = probe_.app_buttons_end()) {
        int apps_end = vertical ? apps->bottom : apps->right;
        if (!scan.buttons_end || apps_end > *scan.buttons_end) scan.buttons_end = apps_end;
        scan.names += std::format(" [app buttons end {} automation]", apps_end);
    }

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

    auto inputs = std::format("Widget placement: bar {}-{}{}, tray {}-{}{}", vertical ? bounds.top : bounds.left,
                              vertical ? bounds.bottom : bounds.right, vertical ? " vertical" : "",
                              vertical ? tray_bounds.top : tray_bounds.left, vertical ? tray_bounds.bottom : tray_bounds.right,
                              notification ? "" : " (estimated)");
    int along = vertical ? y : x;
    if (inputs != last_inputs_ || scan.names != last_names_ || along != last_x_) {
        probe_.request();
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
