#pragma once
#include <windows.h>
#include <chrono>
#include <ctime>
#include <optional>
#include <string>

namespace companion {

// A snapshot stores today's local calendar date and completed seconds in one registry value.
struct DailyTotal {
    DWORD day{};
    DWORD seconds{};
    void advance(std::time_t now, std::chrono::seconds newly_hidden);
};

class Preferences {
public:
    explicit Preferences(const wchar_t* subkey);
    ~Preferences();
    Preferences(const Preferences&) = delete;
    Preferences& operator=(const Preferences&) = delete;
    DWORD shortcut() const;
    void save_shortcut(DWORD choice) const;
    DailyTotal total() const;
    void save_total(DailyTotal value) const;
    bool hide_on_launch() const;
    void save_hide_on_launch(bool enabled) const;
    DWORD taskbar_options() const;
    void save_taskbar_options(DWORD options) const;
    std::wstring notes() const;
    void save_notes(const std::wstring& text) const;
    bool dark_mode() const;
    void save_dark_mode(bool enabled) const;
    DWORD icon_style() const;
    void save_icon_style(DWORD style) const;
    // Bit flags. The away notification and idle warning start on; the Detailed Summary window,
    // which replaces the away notification when chosen, starts off.
    // Hints (first-run and already-running) are on by default; tests turn them off.
    static constexpr DWORD notify_away = 1, notify_idle = 2, notify_detailed = 4, notify_hints = 8, notify_death = 16;
    DWORD notifications() const;
    void save_notifications(DWORD flags) const;
    // Set once the first-run hint has been shown for this Windows account.
    bool first_run_done() const;
    void save_first_run_done() const;
    // Off unless the user turns it on: the check contacts GitHub.
    bool update_check() const;
    void save_update_check(bool enabled) const;
    // The Goals window's last screen position and size.
    std::optional<RECT> goals_bounds() const;
    void save_goals_bounds(const RECT& bounds) const;
    static bool starts_with_windows();
    static void start_with_windows(bool enabled);
private:
    DWORD read_dword(const wchar_t* name, DWORD fallback) const;
    void write_dword(const wchar_t* name, DWORD value, const char* what) const;
    HKEY key_{};
};
}

