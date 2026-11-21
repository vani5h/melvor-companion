#pragma once
#include "taskbar_probe.hpp"
#include <windows.h>
#include <map>
#include <memory>
#include <string>
#include <type_traits>

namespace companion {
// must not be the transparent key RGB(1,1,1)
constexpr COLORREF idle_red = RGB(240, 72, 72);

// Strip on the taskbar next to the tray: switch art plus a short status. Child of Shell_TrayWnd,
// layered with per-pixel alpha. Shrinks to the switch, then hides so the tray icon takes over.
class TaskbarWidget {
public:
    // 28x16 art, centered in a 28 px icon
    static constexpr int art_icon_size = 28;

    enum class Art { awake, asleep, missing, dead };
    struct Look {
        std::wstring text;
        Art art = Art::awake;
        bool alert = false;
        bool operator==(const Look&) const = default;
    };

    TaskbarWidget() = default;
    ~TaskbarWidget();
    TaskbarWidget(const TaskbarWidget&) = delete;
    TaskbarWidget& operator=(const TaskbarWidget&) = delete;

    // clicks post click_message (lParam WM_RBUTTONUP) to owner, which also answers TTN_GETDISPINFOW
    void set_owner(HWND owner, UINT click_message);
    // caller keeps them alive
    void set_art(HICON awake, HICON asleep, HICON missing, HICON dead);

    // false when not shown (no taskbar or no room)
    bool update(const Look& look);
    void remove();
    void theme_changed();
    // layered children of Explorer can miss hover messages; poll about once a second
    void track_hover();

    HWND window() const { return window_; }
    HWND tooltip() const { return tooltip_; }

private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp);
    bool ensure(HWND bar);
    void render(int width, int height, bool with_text);
    SIZE art_size(int max_width, int max_height) const;
    int px(int value) const; // DIP to this DPI
    HFONT font();

    HWND owner_{};
    UINT click_message_{};
    HWND window_{};
    HWND tooltip_{};
    HICON art_[4]{};
    Look shown_;
    bool drawn_{};
    bool current_has_text_{};
    std::unique_ptr<std::remove_pointer_t<HFONT>, decltype(&DeleteObject)> font_{nullptr, DeleteObject};
    UINT font_dpi_{};
    std::map<HWND, RECT> neighbors_;
    TaskbarProbe probe_; // UI Automation
    std::string last_inputs_, last_names_; // for the log
    int last_x_{};
};
}
