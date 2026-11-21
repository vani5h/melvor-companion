#include "detection.hpp"
#include <memory>
#include <system_error>

namespace companion {
std::filesystem::path process_image(DWORD pid)
{
    std::unique_ptr<void, decltype(&CloseHandle)> process(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid), CloseHandle);
    if (!process)
        throw std::system_error(GetLastError(), std::system_category(), "OpenProcess");

    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &size))
        throw std::system_error(GetLastError(), std::system_category(), "QueryFullProcessImageNameW");
    path.resize(size);
    return path;
}
}
