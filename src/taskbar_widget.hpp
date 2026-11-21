#pragma once
#include "taskbar_probe.hpp"
#include <windows.h>
#include <map>
#include <memory>
#include <string>
#include <type_traits>

namespace companion {
// The idle highlight color, shared by the widget and the tray icon. It must not be the widget's
// transparent key color, RGB(1,1,1).
constexpr COLORREF idle_red = RGB(240, 72, 72);

// The optional strip on the taskbar beside the notification area: the switch art plus a short
// status text. It is a child window of Explorer's taskbar (Shell_TrayWnd), drawn as a per-pixel
// alpha layered window, placed in free space between the app buttons and the notification area.
// When there is no room it shrinks to the switch alone, and with no room at all it hides so the
// regular tray icon is the way in.
//
// The tray decides what to show (a Look); this class decides where and how.
class TaskbarWidget {
public:
    // The switch art is 28x16 pixels, centered vertically inside the 28 px icon entry
    // (see tools/build_icons.ps1). Load the art icons at this size.
    static constexpr int art_icon_size = 28;

    enum class Art { awake, asleep, missing, dead };
    struct Look {
        std::wstring text;
        Art art = Art::awake;
        bool alert = false; // red idle box
        bool operator==(const Look&) const = default;
    };

    TaskbarWidget() = default;
    ~TaskbarWidget();
    TaskbarWidget(const TaskbarWidget&) = delete;
    TaskbarWidget& operator=(const TaskbarWidget&) = delete;

    // `owner` (the tray's hidden window) receives `click_message` with lParam WM_RBUTTONUP when
    // the widget is clicked, and the tooltip's TTN_GETDISPINFOW requests for its text.
    void set_owner(HWND owner, UINT click_message);
    // The switch drawings at art_icon_size, owned by the caller and kept alive while set.
    void set_art(HICON awake, HICON asleep, HICON missing, HICON dead);

    // Places and draws the widget for this look. Returns false when it is not shown (no
    // taskbar, or no room), in which case the tray icon is the way in.
    bool update(const Look& look);
    // Destroys the window and tooltip (tray-only mode).
    void remove();
    // Redraws for a theme change (the tooltip follows dark mode).
    void theme_changed();
    // Explorer-parented layered children can miss hover notifications; call about once a second
    // to show or hide the tooltip from the cursor position.
    void track_hover();

    HWND window() const { return window_; }
    HWND tooltip() const { return tooltip_; }

private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp);
    bool ensure(HWND bar);
    void render(int width, int height, bool with_text);
    SIZE art_size(int max_width, int max_height) const;
    int px(int value) const; // device-independent pixels to this widget's DPI
    HFONT font();

    HWND owner_{};
    UINT click_message_{};
    HWND window_{};
    HWND tooltip_{};
    HICON art_[4]{};
    Look shown_;          // what was last drawn
    bool drawn_{};        // shown_ is valid
    bool current_has_text_{}; // the last drawing included the text (not icon-only)
    std::unique_ptr<std::remove_pointer_t<HFONT>, decltype(&DeleteObject)> font_{nullptr, DeleteObject};
    UINT font_dpi_{};
    std::map<HWND, RECT> neighbors_; // taskbar windows kept clear of (see update)
    TaskbarProbe probe_;             // where the app buttons are really drawn (UI Automation)
    std::string last_inputs_, last_names_; // last logged placement (diagnostics)
    int last_x_{};
};
}
