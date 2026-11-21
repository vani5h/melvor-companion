#pragma once
#include <windows.h>
#include <chrono>
#include <optional>
#include <string>
#include <functional>

namespace companion {
void set_dark_mode(bool enabled);
bool dark_mode();
// Opens the Goals window. `bounds` restores the last window position and size; `save_bounds`
// receives them when the window closes.
void edit_goals(HWND owner, const std::wstring& text, std::function<void(const std::wstring&)> save,
                std::optional<RECT> bounds = std::nullopt, std::function<void(const RECT&)> save_bounds = {});
constexpr DWORD default_chord = MAKELONG('M', MOD_CONTROL);
constexpr DWORD disabled_bit = 0x80000000u;
std::wstring chord_label(DWORD binding);
std::optional<DWORD> capture_shortcut(HWND owner, DWORD binding);
bool paint_menu_item(HWND window, const DRAWITEMSTRUCT& item);
bool measure_menu_item(HWND window, MEASUREITEMSTRUCT& item);
void style_menu(HMENU menu);
UINT track_companion_menu(HMENU menu, HWND owner, POINT anchor, UINT flags);
}

