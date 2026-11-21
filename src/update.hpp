#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace companion {
// version from CMakeLists.txt; empty repo hides the update check
std::wstring app_version();
std::wstring update_repo();

// "v1.2.3" -> {1,2,3}; ignores anything after - or +
std::optional<std::array<int, 3>> parse_version(std::wstring_view text);
bool is_newer(std::wstring_view candidate, std::wstring_view current);
std::optional<std::wstring> release_tag(std::string_view json);

struct Release {
    std::wstring tag;
    std::wstring page;
};
// blocks for a few seconds; nullopt if there are no releases
std::optional<Release> fetch_latest_release(const std::wstring& repo);
}
