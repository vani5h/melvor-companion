#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace companion {
// This build's version (from CMakeLists.txt) and the GitHub "owner/repo" to check, which is
// empty until the project is published.
std::wstring app_version();
std::wstring update_repo();

// "v1.2.3" or "1.2" -> {1, 2, 3} / {1, 2, 0}. Anything after '-' or '+' is ignored.
std::optional<std::array<int, 3>> parse_version(std::wstring_view text);
bool is_newer(std::wstring_view candidate, std::wstring_view current);
// The "tag_name" field of a GitHub "latest release" JSON response.
std::optional<std::wstring> release_tag(std::string_view json);

struct Release {
    std::wstring tag;
    std::wstring page; // the release page on github.com
};
// Asks GitHub for the repository's latest release. Blocks for up to a few seconds, so call it
// off the UI thread. Returns nullopt when the repository has no releases; throws on errors.
std::optional<Release> fetch_latest_release(const std::wstring& repo);
}
