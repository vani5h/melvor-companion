#include "log.hpp"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

namespace companion {
void append_log(const std::filesystem::path& file, std::string_view message) noexcept
{
    // One writer at a time keeps the size check and append together, even across sessions.
    HANDLE raw = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return;
    std::unique_ptr<void, decltype(&CloseHandle)> handle(raw, CloseHandle);
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char line[2048]{};
    const int length = std::snprintf(line, sizeof(line), "%04u-%02u-%02uT%02u:%02u:%02uZ %.*s\r\n",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
        static_cast<int>(std::min<size_t>(message.size(), 2000)), message.data());
    LARGE_INTEGER size{}, offset{};
    if (length <= 0 || !GetFileSizeEx(raw, &size)) return;
    constexpr LONGLONG limit = 128 * 1024;
    if (size.QuadPart > limit - length) {
        if (!SetFilePointerEx(raw, offset, nullptr, FILE_BEGIN) || !SetEndOfFile(raw)) return;
    } else if (!SetFilePointerEx(raw, offset, nullptr, FILE_END)) return;
    DWORD written{};
    WriteFile(raw, line, static_cast<DWORD>(length), &written, nullptr);
}

void log_event(std::string_view message) noexcept
{
    try {
        static const auto file = [] {
            PWSTR raw{};
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)))
                return std::filesystem::path{};
            std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(raw, CoTaskMemFree);
            auto dir = std::filesystem::path(raw) / L"Melvor Companion";
            std::filesystem::create_directories(dir);
            return dir / L"companion.log";
        }();
        if (!file.empty()) append_log(file, message);
    } catch (...) {
        OutputDebugStringW(L"Melvor Companion: diagnostic log unavailable.\n");
    }
}
}
