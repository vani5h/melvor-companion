#include "detection.hpp"
#include <exception>
#include <system_error>

namespace companion {
namespace {
struct Search {
    const std::filesystem::path& image;
    const std::wstring& window_class;
    WindowSearchResult result;
    std::exception_ptr error;
};

BOOL CALLBACK inspect(HWND window, LPARAM context) noexcept
{
    auto& search = *reinterpret_cast<Search*>(context);
    try {
        wchar_t name[256]{};
        if (GetWindow(window, GW_OWNER) || !GetClassNameW(window, name, 256) || search.window_class != name)
            return TRUE;
        DWORD pid{};
        if (!GetWindowThreadProcessId(window, &pid)) return TRUE;
        auto image = process_image(pid);
        if (!std::filesystem::equivalent(image, search.image)) return TRUE;
        std::wstring title(GetWindowTextLengthW(window) + 1, L'\0');
        title.resize(GetWindowTextW(window, title.data(), static_cast<int>(title.size())));
        DWORD current_pid{};
        GetWindowThreadProcessId(window, &current_pid);
        if (current_pid == pid)
            search.result.matches.push_back({window, pid, std::move(image), name, std::move(title)});
    } catch (const std::system_error& e) {
        // A window can disappear between enumeration and querying its process.
        if (e.code().value() == ERROR_INVALID_PARAMETER || !IsWindow(window)) return TRUE;
        if (e.code() == std::error_code(ERROR_ACCESS_DENIED, std::system_category())) {
            ++search.result.inaccessible;
            return TRUE;
        }
        search.error = std::current_exception();
        return FALSE;
    } catch (...) {
        search.error = std::current_exception();
        return FALSE;
    }
    return TRUE;
}
}

WindowSearchResult find_windows(const std::filesystem::path& image, const std::wstring& window_class)
{
    Search search{image, window_class, {}, {}};
    if (!EnumWindows(inspect, reinterpret_cast<LPARAM>(&search))) {
        if (search.error) std::rethrow_exception(search.error);
        throw std::system_error(GetLastError(), std::system_category(), "EnumWindows");
    }
    return std::move(search.result);
}
}
