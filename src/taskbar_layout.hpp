#pragma once
#include <windows.h>
#include <map>
#include <optional>

namespace companion {
struct WidgetPlacement { int x, width; };

// screen coordinates, x can be negative. Empty result = use the tray icon.
inline std::optional<WidgetPlacement> fit_taskbar_widget(int left, int right, int full_width,
        int icon_width, const std::map<HWND, RECT>& occupied, int gap = 8) {
    // 8 px at 100%
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

// Vertical taskbars: transpose x/y and reuse the horizontal search.
// The result x is really y, and width is height.
inline std::optional<WidgetPlacement> fit_taskbar_widget_vertical(int top, int bottom, int height,
        const std::map<HWND, RECT>& occupied, int gap = 8) {
    std::map<HWND, RECT> transposed;
    for (const auto& [window, rect] : occupied)
        transposed[window] = RECT{rect.top, rect.left, rect.bottom, rect.right};
    return fit_taskbar_widget(top, bottom, height, height, transposed, gap);
}
}
