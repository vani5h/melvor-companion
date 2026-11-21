#pragma once

#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

namespace companion {
struct WindowInfo {
    HWND handle;
    DWORD pid;
    std::filesystem::path image;
    std::wstring window_class;
    std::wstring title;
};

inline constexpr wchar_t melvor_class[] = L"Engine";
std::filesystem::path melvor_image(std::filesystem::path steam_directory = {});
std::filesystem::path process_image(DWORD pid);
struct WindowSearchResult {
    std::vector<WindowInfo> matches;
    unsigned inaccessible{};
};
WindowSearchResult find_windows(const std::filesystem::path& image, const std::wstring& window_class);
}
