#include "detection.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace companion {
namespace {
fs::path steam_root()
{
    wchar_t path[32768]{};
    DWORD bytes = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, path, &bytes) == ERROR_SUCCESS)
        return path;
    bytes = sizeof(path);
    auto status = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath",
                               RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, nullptr, path, &bytes);
    if (status != ERROR_SUCCESS)
        throw std::system_error(status, std::system_category(), "Locate Steam installation");
    return path;
}

// Steam writes one quoted key/value pair per line. std::quoted decodes escaped backslashes.
std::vector<std::string> values(const fs::path& file, const std::string& key)
{
    std::ifstream input(file);
    if (!input) throw std::runtime_error("Cannot open Steam metadata: " + file.string());
    std::vector<std::string> result;
    for (std::string line; std::getline(input, line);) {
        std::istringstream fields(line);
        std::string field, value;
        if (fields >> std::quoted(field) && field == key && fields >> std::quoted(value))
            result.push_back(std::move(value));
    }
    if (input.bad()) throw std::runtime_error("Cannot read Steam metadata: " + file.string());
    return result;
}

fs::path utf8_path(const std::string& value)
{
    return fs::path(std::u8string(value.begin(), value.end()));
}
}

fs::path melvor_image(fs::path root)
{
    if (root.empty()) root = steam_root();
    std::vector<fs::path> libraries{root};
    for (const auto& value : values(root / "steamapps/libraryfolders.vdf", "path"))
        libraries.push_back(utf8_path(value));

    fs::path found;
    for (const auto& library : libraries) {
        const auto apps = library / "steamapps";
        const auto manifest = apps / "appmanifest_3218350.acf";
        if (!fs::exists(manifest)) continue;
        if (values(manifest, "appid") != std::vector<std::string>{"3218350"})
            throw std::runtime_error("Unexpected Steam app ID in Melvor manifest");
        const auto dirs = values(manifest, "installdir");
        if (dirs.size() != 1) throw std::runtime_error("Missing or ambiguous Steam installdir");
        auto image = apps / "common" / utf8_path(dirs.front()) / "Melvor2.exe";
        if (!fs::is_regular_file(image)) continue;
        if (!found.empty() && !fs::equivalent(found, image))
            throw std::runtime_error("Multiple Melvor installations found");
        found = std::move(image);
    }
    if (found.empty()) throw std::runtime_error("Melvor2.exe missing from registered Steam libraries");
    return found;
}
}
