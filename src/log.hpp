#pragma once
#include <filesystem>
#include <string_view>

namespace companion {
void log_event(std::string_view message) noexcept;
void append_log(const std::filesystem::path& file, std::string_view message) noexcept;
}
