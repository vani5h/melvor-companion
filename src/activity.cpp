#include "activity.hpp"
#include "settings.hpp"
#include <algorithm>
#include <system_error>

namespace companion {
DWORD Preferences::read_dword(const wchar_t* name, DWORD fallback) const {
    DWORD value{}, size = sizeof(value);
    return RegGetValueW(key_, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : fallback;
}
void Preferences::write_dword(const wchar_t* name, DWORD value, const char* what) const {
    auto status = RegSetValueExW(key_, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    if (status) throw std::system_error(status, std::system_category(), what);
}
DWORD Preferences::notifications() const {
    DWORD flags = read_dword(L"Notifications", notify_away | notify_idle | notify_hints | notify_death);
    // options added later default on; marker bits 0x80 hints, 0x100 death
    if (!(flags & 0x80)) flags |= notify_hints;
    if (!(flags & 0x100)) flags |= notify_death;
    return flags & 31;
}
void Preferences::save_notifications(DWORD flags) const {
    write_dword(L"Notifications", (flags & 31) | 0x80 | 0x100, "Save notification settings");
}
bool Preferences::first_run_done() const { return read_dword(L"FirstRunDone", 0) != 0; }
void Preferences::save_first_run_done() const { write_dword(L"FirstRunDone", 1, "Save first-run state"); }
bool Preferences::update_check() const { return read_dword(L"UpdateCheck", 0) != 0; }
void Preferences::save_update_check(bool enabled) const { write_dword(L"UpdateCheck", enabled, "Save update setting"); }
std::optional<RECT> Preferences::goals_bounds() const {
    RECT bounds{};
    DWORD size = sizeof(bounds);
    if (RegGetValueW(key_, nullptr, L"GoalsBounds", RRF_RT_REG_BINARY, nullptr, &bounds, &size) != ERROR_SUCCESS
        || size != sizeof(bounds) || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return std::nullopt;
    return bounds;
}
void Preferences::save_goals_bounds(const RECT& bounds) const {
    auto status = RegSetValueExW(key_, L"GoalsBounds", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&bounds), sizeof(bounds));
    if (status) throw std::system_error(status, std::system_category(), "Save Goals window position");
}
DWORD Preferences::icon_style() const {
    DWORD value=0,size=sizeof(value);
    if(RegGetValueW(key_,nullptr,L"IconStyle",RRF_RT_REG_DWORD,nullptr,&value,&size)!=ERROR_SUCCESS) return 0;
    return value<4 ? value : 0;
}
void Preferences::save_icon_style(DWORD value) const {
    auto status=RegSetValueExW(key_,L"IconStyle",0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value));
    if(status) throw std::system_error(status,std::system_category(),"Save icon style");
}
bool Preferences::dark_mode() const {
    DWORD value=1, size=sizeof(value);
    if (RegGetValueW(key_, nullptr, L"DarkMode", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return true;
    return value != 0;
}
void Preferences::save_dark_mode(bool enabled) const {
    DWORD value=enabled;
    auto status=RegSetValueExW(key_, L"DarkMode", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    if (status) throw std::system_error(status, std::system_category(), "Save theme");
}
std::wstring Preferences::notes() const {
    DWORD bytes{};
    auto status = RegGetValueW(key_, nullptr, L"Goals", RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return {};
    if (status || bytes > 131074) throw std::runtime_error("Could not read saved goals");
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    status = RegGetValueW(key_, nullptr, L"Goals", RRF_RT_REG_SZ, nullptr, text.data(), &bytes);
    if (status) throw std::system_error(status, std::system_category(), "Read goals");
    text.resize(bytes / sizeof(wchar_t));
    text.resize(text.find(L'\0') == std::wstring::npos ? text.size() : text.find(L'\0'));
    return text;
}
void Preferences::save_notes(const std::wstring& text) const {
    if (text.size() > 65536) throw std::runtime_error("Goals exceed the text limit");
    auto status = RegSetValueExW(key_, L"Goals", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(text.c_str()), static_cast<DWORD>((text.size()+1)*sizeof(wchar_t)));
    if (status) throw std::system_error(status, std::system_category(), "Save goals");
}
DWORD Preferences::taskbar_options() const {
    DWORD value = 2, size = sizeof(value);
    if (RegGetValueW(key_, nullptr, L"TaskbarOptions", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return 2;
    return value & 31;
}
void Preferences::save_taskbar_options(DWORD value) const {
    auto status = RegSetValueExW(key_, L"TaskbarOptions", 0, REG_DWORD,
                               reinterpret_cast<const BYTE*>(&value), sizeof(value));
    if (status) throw std::system_error(status, std::system_category(), "Save taskbar options");
}
bool Preferences::hide_on_launch() const {
    DWORD value{}, size = sizeof(value);
    return RegGetValueW(key_, nullptr, L"HideOnLaunch", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value;
}
void Preferences::save_hide_on_launch(bool enabled) const {
    DWORD value = enabled;
    auto status = RegSetValueExW(key_, L"HideOnLaunch", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    if (status) throw std::system_error(status, std::system_category(), "Save game launch preference");
}
bool Preferences::starts_with_windows() {
    DWORD bytes{};
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        L"Melvor Companion", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) == ERROR_SUCCESS;
}
void Preferences::start_with_windows(bool enabled) {
    HKEY key{};
    auto status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                  0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status) throw std::system_error(status, std::system_category(), "Open Windows startup settings");
    if (enabled) {
        std::wstring path(32768, L'\0');
        DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) { RegCloseKey(key); throw std::runtime_error("Cannot locate companion executable"); }
        path.resize(length);
        path = L"\"" + path + L"\"";
        status = RegSetValueExW(key, L"Melvor Companion", 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(path.c_str()), static_cast<DWORD>((path.size()+1)*sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, L"Melvor Companion");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (status) throw std::system_error(status, std::system_category(), "Update Windows startup setting");
}
void DailyTotal::advance(std::time_t now, std::chrono::seconds newly_hidden)
{
    std::tm local{};
    if (localtime_s(&local, &now)) return;
    auto today = static_cast<DWORD>((1900 + local.tm_year) * 10000 + (1 + local.tm_mon) * 100 + local.tm_mday);
    auto increment = newly_hidden.count();
    if (day != today) {
        day = today;
        seconds = 0;
        local.tm_hour = local.tm_min = local.tm_sec = 0;
        local.tm_isdst = -1;
        auto midnight = std::mktime(&local);
        increment = std::min<long long>(increment, now - midnight);
    }
    seconds += static_cast<DWORD>(std::max(0LL, increment));
}

namespace {
void check(LSTATUS status, const char* operation)
{
    if (status != ERROR_SUCCESS) throw std::system_error(status, std::system_category(), operation);
}
}

Preferences::Preferences(const wchar_t* subkey)
{
    check(RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                          nullptr, &key_, nullptr), "Open Melvor Companion settings");
}
Preferences::~Preferences() { RegCloseKey(key_); }

DWORD Preferences::shortcut() const
{
    DWORD value{}, size = sizeof(value);
    if (RegGetValueW(key_, nullptr, L"KeyBinding", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS
        && LOWORD(value) && LOWORD(value) <= 255 && !(value & 0x7ff00000u)) return value;
    size = sizeof(value);
    if (RegGetValueW(key_, nullptr, L"Shortcut", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        constexpr DWORD legacy[]{default_chord | disabled_bit, MAKELONG('M', MOD_CONTROL | MOD_ALT),
            MAKELONG('M', MOD_CONTROL | MOD_SHIFT), MAKELONG('M', MOD_ALT | MOD_SHIFT)};
        if (value < 4) return legacy[value];
    }
    return default_chord;
}
void Preferences::save_shortcut(DWORD choice) const
{
    check(RegSetValueExW(key_, L"KeyBinding", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&choice),
                         sizeof(choice)), "Save shortcut");
}
DailyTotal Preferences::total() const
{
    ULONGLONG value{};
    DWORD size = sizeof(value);
    if (RegGetValueW(key_, nullptr, L"DailyTotal", RRF_RT_REG_QWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return {};
    return {static_cast<DWORD>(value >> 32), static_cast<DWORD>(value)};
}
void Preferences::save_total(DailyTotal total) const
{
    ULONGLONG value = (static_cast<ULONGLONG>(total.day) << 32) | total.seconds;
    check(RegSetValueExW(key_, L"DailyTotal", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&value),
                         sizeof(value)), "Save time in tray");
}
}
