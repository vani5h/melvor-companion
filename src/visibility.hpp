#pragma once
#include "detection.hpp"
#include "audio.hpp"
#include <chrono>
#include <optional>

namespace companion {
class HiddenWindow {
public:
    HiddenWindow() = default;
    HiddenWindow(const HiddenWindow&) = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;
    ~HiddenWindow() { restore(); }

    bool owns_window() const noexcept;
    std::optional<std::chrono::seconds> hidden_duration() const noexcept;
    bool hide(const WindowInfo& target);
    bool recover_previous_hide(const WindowInfo& target);
    bool restore() noexcept;
    void refresh_audio() noexcept;

private:
    HWND window_{};
    DWORD pid_{};
    HiddenAudio audio_;
    std::chrono::steady_clock::time_point hidden_since_{};
};
}
