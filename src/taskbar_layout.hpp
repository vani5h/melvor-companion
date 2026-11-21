#pragma once
#include <windows.h>
#include <map>
#include <optional>

namespace companion {
struct WidgetPlacement { int x, width; };

// Search only between the task buttons and notification area. Positions are screen
// coordinates, so negative x is valid. An empty result leaves the shell tray icon in use.
inline std::optional<WidgetPlacement> fit_taskbar_widget(int left, int right, int full_width,
        int icon_width, const std::map<HWND, RECT>& occupied, int gap = 8) {
    // gap: space kept from each neighbor, in pixels (8 at 100% display scaling).
    for (int width : {full_width, icon_width}) {
        int x = right - gap - width;
        while (x >= left + gap) {
            int previous = x;
            for (const auto& [window, rect] : occupied) {
                if (x < rect.right + gap && x + width + gap > rect.left)
                    x = rect.left - gap - width;
            }
            if (previous == x) return WidgetPlacement{x, width};
        }
    }
    return std::nullopt;
}

// Vertical taskbars (docked left or right): the same search along the y axis, between the
// bottom of the app buttons (`top`) and the top of the notification area (`bottom`). Each
// rectangle is transposed (x and y swapped) so the horizontal search can be reused unchanged.
// The result's x is a y coordinate and its width a height.
inline std::optional<WidgetPlacement> fit_taskbar_widget_vertical(int top, int bottom, int height,
        const std::map<HWND, RECT>& occupied, int gap = 8) {
    std::map<HWND, RECT> transposed;
    for (const auto& [window, rect] : occupied)
        transposed[window] = RECT{rect.top, rect.left, rect.bottom, rect.right};
    return fit_taskbar_widget(top, bottom, height, height, transposed, gap);
}
}
