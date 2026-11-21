#pragma once
#include "detection.hpp"
#include <chrono>
#include <optional>

namespace companion {
std::wstring tray_tooltip(std::optional<std::chrono::seconds> elapsed);
int run_tray(HINSTANCE instance, const std::filesystem::path& image = {},
             const wchar_t* window_class = melvor_class,
             const wchar_t* mutex_name = L"Local\\MelvorCompanion.Tray.29C100B9",
             const wchar_t* settings_key = L"Software\\Melvor Companion");
}
