#include "visibility.hpp"
#include <system_error>

namespace companion {
namespace {
constexpr wchar_t marker[] = L"MelvorCompanion.HiddenWindow.29C100B9";
bool matches_target(const WindowInfo& target)
{
    DWORD pid{};
    wchar_t name[256]{};
    return GetWindowThreadProcessId(target.handle, &pid) && pid == target.pid &&
        !GetWindow(target.handle, GW_OWNER) && GetClassNameW(target.handle, name, 256) &&
        target.window_class == name && std::filesystem::equivalent(process_image(pid), target.image);
}
}
bool HiddenWindow::owns_window() const noexcept
{
    DWORD pid{};
    return window_ && GetWindowThreadProcessId(window_, &pid) && pid == pid_
        && GetPropW(window_, marker) == this;
}

std::optional<std::chrono::seconds> HiddenWindow::hidden_duration() const noexcept
{
    if (!owns_window() || IsWindowVisible(window_)) return std::nullopt;
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - hidden_since_);
}
void HiddenWindow::refresh_audio() noexcept {
    if (owns_window() && !IsWindowVisible(window_)) audio_.mute(pid_);
    else audio_.restore();
}

bool HiddenWindow::hide(const WindowInfo& target)
{
    if (owns_window()) return window_ == target.handle && !IsWindowVisible(window_);
    audio_.restore();
    HWND window = target.handle;
    if (!IsWindowVisible(window) || !matches_target(target)) return false;
    if (!SetPropW(window, marker, this))
        throw std::system_error(GetLastError(), std::system_category(), "SetPropW");
    window_ = window;
    pid_ = target.pid;
    ShowWindow(window, SW_HIDE);
    if (owns_window() && !IsWindowVisible(window)) {
        hidden_since_ = std::chrono::steady_clock::now();
        audio_.mute(pid_);
        return true;
    }
    if (owns_window()) RemovePropW(window, marker);
    window_ = nullptr;
    return false;
}

bool HiddenWindow::recover_previous_hide(const WindowInfo& target)
{
    if (owns_window() || !GetPropW(target.handle, marker) || !matches_target(target)) return false;
    // the old value belongs to the dead process: never dereference it
    if (!SetPropW(target.handle, marker, this)) return false;
    window_ = target.handle;
    pid_ = target.pid;
    hidden_since_ = std::chrono::steady_clock::now();
    return restore();
}

bool HiddenWindow::restore() noexcept
{
    if (owns_window()) {
        // behind the user's current window
        if (!IsWindowVisible(window_) &&
            (!SetWindowPos(window_, HWND_BOTTOM, 0, 0, 0, 0,
                           SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE)
             || !IsWindowVisible(window_))) return false;
        RemovePropW(window_, marker);
    }
    window_ = nullptr;
    audio_.restore();
    return true;
}
}
