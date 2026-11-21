#include "update.hpp"
#include "version.h"
#include <windows.h>
#include <winhttp.h>
#include <memory>
#include <stdexcept>

namespace companion {
std::wstring app_version() { return MELVOR_SWITCH_VERSION; }
std::wstring update_repo() { return MELVOR_COMPANION_UPDATE_REPO; }

std::optional<std::array<int, 3>> parse_version(std::wstring_view text) {
    if (!text.empty() && (text.front() == L'v' || text.front() == L'V')) text.remove_prefix(1);
    std::array<int, 3> parts{};
    size_t part = 0;
    bool digits = false;
    for (wchar_t c : text) {
        if (c >= L'0' && c <= L'9') {
            if (parts[part] > 100000) return std::nullopt;
            parts[part] = parts[part] * 10 + (c - L'0');
            digits = true;
        } else if (c == L'.' && digits) {
            if (part == 2) break; // a fourth part (1.2.3.4) does not change the comparison
            ++part;
            digits = false;
        } else if ((c == L'-' || c == L'+') && digits) {
            break;
        } else {
            return std::nullopt;
        }
    }
    if (!digits) return std::nullopt;
    return parts;
}

bool is_newer(std::wstring_view candidate, std::wstring_view current) {
    auto a = parse_version(candidate), b = parse_version(current);
    return a && b && *a > *b; // std::array compares element by element
}

std::optional<std::wstring> release_tag(std::string_view json) {
    // A full JSON parser is unnecessary for one known field. Tags are short and plain, so
    // anything unusual (escapes, long values) is rejected rather than interpreted.
    auto key = json.find("\"tag_name\"");
    if (key == std::string_view::npos) return std::nullopt;
    auto at = json.find_first_not_of(" \t\r\n", key + 10);
    if (at == std::string_view::npos || json[at] != ':') return std::nullopt;
    at = json.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string_view::npos || json[at] != '"') return std::nullopt;
    auto end = json.find('"', at + 1);
    if (end == std::string_view::npos || end - at - 1 > 64) return std::nullopt;
    auto value = json.substr(at + 1, end - at - 1);
    if (value.empty() || value.find('\\') != std::string_view::npos) return std::nullopt;
    std::wstring tag;
    for (char c : value) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) return std::nullopt;
        tag += static_cast<wchar_t>(c);
    }
    return tag;
}

namespace {
struct InternetDeleter { void operator()(HINTERNET handle) const { WinHttpCloseHandle(handle); } };
using Internet = std::unique_ptr<void, InternetDeleter>;
}

std::optional<Release> fetch_latest_release(const std::wstring& repo) {
    if (repo.empty() || repo.find(L'/') == std::wstring::npos) return std::nullopt;
    // GitHub requires a User-Agent; it names the app and version only.
    auto agent = L"MelvorCompanion/" + app_version();
    Internet session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) throw std::runtime_error("Could not start the update check");
    WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 5000);
    Internet connection(WinHttpConnect(session.get(), L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) throw std::runtime_error("Could not reach GitHub");
    auto path = L"/repos/" + repo + L"/releases/latest";
    Internet request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) throw std::runtime_error("Could not reach GitHub");
    if (!WinHttpSendRequest(request.get(), L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request.get(), nullptr))
        throw std::runtime_error("Could not reach GitHub");
    DWORD status{}, size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status == 404) return std::nullopt; // no published release yet
    if (status != 200) throw std::runtime_error("GitHub returned HTTP " + std::to_string(status));
    std::string body;
    for (;;) {
        DWORD available{};
        if (!WinHttpQueryDataAvailable(request.get(), &available)) throw std::runtime_error("Update check interrupted");
        if (!available) break;
        if (body.size() + available > 1024 * 1024) throw std::runtime_error("Unexpected GitHub response");
        auto old = body.size();
        body.resize(old + available);
        DWORD read{};
        if (!WinHttpReadData(request.get(), body.data() + old, available, &read)) throw std::runtime_error("Update check interrupted");
        body.resize(old + read);
    }
    auto tag = release_tag(body);
    if (!tag) throw std::runtime_error("Unexpected GitHub response");
    return Release{*tag, L"https://github.com/" + repo + L"/releases/tag/" + *tag};
}
}
