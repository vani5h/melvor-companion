#include "visibility.hpp"
#include "tray.hpp"
#include "taskbar_layout.hpp"
#include "log.hpp"
#include "activity.hpp"
#include "game_activity.hpp"
#include "settings.hpp"
#include "update.hpp"
#include "summary.hpp"
#include "taskbar_widget.hpp"
#include "death_tracker.hpp"
#include "death_panel.hpp"
#include <future>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>
#include <string>
#include <system_error>
#include <format>
#include <optional>
#include <commctrl.h>
#include <uxtheme.h>

std::wstring companion::tray_tooltip(std::optional<std::chrono::seconds> elapsed)
{
    if (!elapsed) return L"Melvor Companion";
    const std::chrono::hh_mm_ss time(*elapsed);
    return std::format(L"Melvor Companion\nTime Minimized: {}:{:02}:{:02}",
                       time.hours().count(), time.minutes().count(), time.seconds().count());
}

namespace {
constexpr UINT tray_event = WM_APP + 1;
constexpr UINT game_event = WM_APP + 2;
thread_local HWND event_host{};
void CALLBACK game_changed(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD) {
    if (window && object == OBJID_WINDOW && child == CHILDID_SELF)
        PostMessageW(event_host, game_event, event, reinterpret_cast<LPARAM>(window));
}
constexpr UINT update_event = WM_APP + 3;
// registered, not WM_APP: other copies of the app receive it
UINT already_running_message() {
    static const UINT message = RegisterWindowMessageW(L"MelvorSwitch.AlreadyRunning");
    return message;
}
constexpr UINT_PTR tooltip_timer = 1;
constexpr UINT_PTR update_timer = 2;
constexpr UINT_PTR death_timer = 3;
constexpr UINT update_interval_ms = 24 * 60 * 60 * 1000;
constexpr const wchar_t* steam_launch = L"steam://rungameid/3218350";

enum Command : UINT { hide_game = 1, restore_game, quit, toggle_keys = 10, configure_key,
                      toggle_startup, toggle_launch, toggle_taskbar, widget_session, widget_today, widget_status,
                      goals, toggle_dark, widget_activity, style_teal=30, style_blue, style_lavender, style_states,
                      launch_game = 40, toggle_away, toggle_idle, toggle_updates, open_update, toggle_detailed, about_app, toggle_death };


// copy of base with a solid frame; caller destroys it
HICON framed_icon(HICON base, COLORREF frame) {
    ICONINFO info{};
    if (!GetIconInfo(base, &info)) return nullptr;
    // GetIconInfo returns copies
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)> source_color(info.hbmColor, DeleteObject);
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)> source_mask(info.hbmMask, DeleteObject);
    BITMAP shape{};
    if (!info.hbmColor || !GetObjectW(info.hbmColor, sizeof(shape), &shape)) return nullptr;
    int width = shape.bmWidth, height = shape.bmHeight;
    BITMAPINFO format{};
    format.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB}; // top-down
    std::vector<DWORD> pixels(static_cast<size_t>(width) * height);
    HDC screen = GetDC(nullptr);
    bool read = GetDIBits(screen, info.hbmColor, 0, height, pixels.data(), &format, DIB_RGB_COLORS) == height;
    void* bits{};
    HBITMAP color = read ? CreateDIBSection(screen, &format, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)> framed_color(color, DeleteObject);
    int thickness = std::max(1, width / 16);
    DWORD red = 0xFF000000u | (GetRValue(frame) << 16) | (GetGValue(frame) << 8) | GetBValue(frame);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (x < thickness || y < thickness || x >= width - thickness || y >= height - thickness)
                pixels[static_cast<size_t>(y) * width + x] = red;
    std::copy(pixels.begin(), pixels.end(), static_cast<DWORD*>(bits));
    // ignored with per-pixel alpha, but required
    std::vector<BYTE> zero(static_cast<size_t>((width + 15) / 16 * 2) * height);
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)> mask(
        CreateBitmap(width, height, 1, 1, zero.data()), DeleteObject);
    ICONINFO made{TRUE, 0, 0, mask.get(), framed_color.get()};
    return CreateIconIndirect(&made);
}

// the worker keeps a reference, so this can outlive the tray
struct UpdateState {
    std::mutex lock;
    bool running{};
    std::optional<companion::Release> release;
    std::string error;
};

void show_about(HWND owner) {
    std::wstring logs;
    PWSTR raw{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
        logs = std::wstring(raw) + L"\\Melvor Companion"; // old folder name, kept so logs stay in one place
        CoTaskMemFree(raw);
    }
    auto repo = companion::update_repo();
    auto title = L"Melvor Companion " + companion::app_version();
    std::wstring content =
        L"An unofficial companion for Melvor Idle 2. It is not affiliated with or endorsed by Games by Malcs; "
        L"Melvor Idle is their trademark.\n\n"
        L"It reads your local save files and never changes them. It only goes online if you turn on "
        L"Check for updates.\n\n"
        L"Free and open source under the MIT License. Includes the Brotli decoder (MIT).";
    std::wstring footer = L"<a href=\"logs\">Open the log folder</a>";
    if (!repo.empty()) footer = L"<a href=\"https://github.com/" + repo + L"\">Project page and releases</a>    " + footer;
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = owner;
    config.hInstance = GetModuleHandleW(nullptr);
    config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_HICON_MAIN | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = L"About Melvor Companion";
    config.hMainIcon = static_cast<HICON>(LoadImageW(config.hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                     GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    config.pszMainInstruction = title.c_str();
    config.pszContent = content.c_str();
    config.pszFooter = footer.c_str();
    config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&logs);
    config.pfCallback = [](HWND, UINT notification, WPARAM, LPARAM lp, LONG_PTR data) -> HRESULT {
        if (notification == TDN_HYPERLINK_CLICKED) {
            std::wstring target = reinterpret_cast<const wchar_t*>(lp);
            if (target == L"logs") target = *reinterpret_cast<const std::wstring*>(data);
            if (!target.empty()) ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return S_OK;
    };
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

using Menu = std::unique_ptr<std::remove_pointer_t<HMENU>, decltype(&DestroyMenu)>;
using OwnedIcon = std::unique_ptr<std::remove_pointer_t<HICON>, decltype(&DestroyIcon)>;
void append(HMENU menu, UINT flags, UINT_PTR id, const wchar_t* text) {
    if (!AppendMenuW(menu, flags, id, text)) throw std::runtime_error("Could not append tray menu item.");
}

std::runtime_error api_failure(const char* api, DWORD code) {
    return std::runtime_error(std::string(api) + " failed (Win32 " + std::to_string(code)
                              + "): " + std::system_category().message(static_cast<int>(code)));
}
struct Tray {
    companion::HiddenWindow hidden;
    std::filesystem::path image;
    const wchar_t* window_class;
    companion::Preferences prefs;
    companion::DailyTotal total = prefs.total();
    companion::DailyTotal saved = total;
    std::chrono::seconds counted{};

    DWORD shortcut = companion::default_chord | companion::disabled_bit;
    int hotkey_id{};
    HICON normal_icon{}, hidden_icon{}, missing_icon{}, dead_icon{};
    DWORD icon_style = prefs.icon_style();

    void load_icons() {
        HINSTANCE instance=GetModuleHandleW(nullptr);
        auto load=[&](WORD id) {
            return static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(id),IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
        };
        normal_icon=load(static_cast<WORD>(101+icon_style*2));
        hidden_icon=load(static_cast<WORD>(102+icon_style*2));
        missing_icon=load(109);
        dead_icon=load(110);
        // 28 px isn't a system icon size, so no LR_SHARED
        auto load_art=[&](WORD id) {
            return OwnedIcon(static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(id),IMAGE_ICON,
                companion::TaskbarWidget::art_icon_size,companion::TaskbarWidget::art_icon_size,0)),DestroyIcon);
        };
        widget_art_normal=load_art(static_cast<WORD>(101+icon_style*2));
        widget_art_hidden=load_art(static_cast<WORD>(102+icon_style*2));
        widget_art_missing=load_art(109);
        widget_art_dead=load_art(110);
        widget.set_art(widget_art_normal.get(), widget_art_hidden.get(), widget_art_missing.get(), widget_art_dead.get());
        idle_icon=OwnedIcon(framed_icon(hidden_icon, companion::idle_red),DestroyIcon);
        if(!normal_icon || !hidden_icon || !missing_icon || !dead_icon || !widget_art_normal || !widget_art_hidden
           || !widget_art_missing || !widget_art_dead || !idle_icon)
            throw std::runtime_error("Could not load switch icons");
    }
    OwnedIcon widget_art_normal{nullptr, DestroyIcon}, widget_art_hidden{nullptr, DestroyIcon};
    OwnedIcon widget_art_missing{nullptr, DestroyIcon}, widget_art_dead{nullptr, DestroyIcon};
    OwnedIcon idle_icon{nullptr, DestroyIcon};

    HICON state_icon(bool hidden_now) const {
        if (!hidden_now && observed.empty()) return missing_icon;
        if (died()) return dead_icon;
        if (!hidden_now) return normal_icon;
        return idle_now ? idle_icon.get() : hidden_icon;
    }

    NOTIFYICONDATAW icon{};
    bool icon_hidden{};
    UINT explorer_started = RegisterWindowMessageW(L"TaskbarCreated");
    bool timing_session{};
    bool hide_launch = prefs.hide_on_launch();
    std::map<HWND, bool> observed; // window -> first launch handled
    DWORD taskbar = prefs.taskbar_options();
    companion::GameActivity activity;
    companion::TaskbarWidget widget;
    std::wstring hover_text;
    DWORD notify_flags = prefs.notifications();
    std::optional<companion::ProgressSnapshot> away_baseline;
    bool idle_warned{};
    bool idle_now{};
    companion::DeathTracker death_tracker;
    std::future<bool> panel_result;
    companion::DeathTracker::Id panel_character{};
    HWND panel_window{};
    DWORD panel_pid{};
    std::chrono::steady_clock::time_point next_panel_check{};
    bool death_warned{};
    using SaveTime = std::filesystem::file_time_type;
    std::optional<SaveTime> hide_start, death_checked_save;
    bool died() const { return (notify_flags & companion::Preferences::notify_death) && death_tracker.dead(); }
    bool check_updates = prefs.update_check();
    std::shared_ptr<UpdateState> update_state = std::make_shared<UpdateState>();
    std::optional<companion::Release> available_update;
    std::wstring announced_update;

    void notify(const std::wstring& title, const std::wstring& text) {
        NOTIFYICONDATAW note = icon;
        note.uFlags = NIF_INFO;
        wcsncpy_s(note.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(note.szInfo, text.c_str(), _TRUNCATE);
        note.dwInfoFlags = NIIF_NONE | NIIF_RESPECT_QUIET_TIME;
        if (!Shell_NotifyIconW(NIM_MODIFY, &note)) companion::log_event("Could not show a notification.");
    }

    void began_hiding() {
        idle_warned = false;
        if (!(notify_flags & companion::Preferences::notify_death)) death_tracker.clear();
        hide_start = SaveTime::clock::now();
        death_tracker.begin_hidden(*hide_start);
        capture_baseline();
        if (away_baseline) {
            observe_progress(*away_baseline);
            death_tracker.seed_count(away_baseline->deaths);
        }
    }

    void capture_baseline() {
        away_baseline.reset();
        if (!(notify_flags & (companion::Preferences::notify_away | companion::Preferences::notify_detailed))) return;
        try {
            if (auto save = companion::newest_save()) away_baseline = companion::read_progress_save(save->path);
        } catch (const std::exception& error) {
            companion::log_event(std::string("Away summary: could not read the save when hiding: ") + error.what());
        }
    }

    void report_away(std::chrono::seconds hidden_for, SaveTime shown_at) {
        if (!away_baseline) return;
        if (!(notify_flags & (companion::Preferences::notify_away | companion::Preferences::notify_detailed))) { away_baseline.reset(); return; }
        auto baseline = std::move(*away_baseline);
        away_baseline.reset();
        try {
            auto save = companion::newest_save();
            if (!save) return;
            auto now = companion::read_progress_save(save->path);
            if (baseline.activity.save_id != now.activity.save_id) return;
            observe_progress(now);
            bool new_save = now.saved_at != baseline.saved_at;
            auto change = companion::compare_progress(baseline, now);
            change.possible_thieving_death = death_tracker.hidden_thieving_death() && !change.deaths;
            change.idle_after_death = death_tracker.hidden_idle(shown_at);
            if (change.idle_after_death) {
                // includes a death just before hiding
                change.deaths = std::max<long long>(change.deaths, 1);
            }
            if (!change.deaths && hidden_for < std::chrono::minutes(1)) return;
            if (notify_flags & companion::Preferences::notify_detailed) {
                companion::show_summary(change, hidden_for, new_save, normal_icon);
                return;
            }
            std::wstring text;
            if (!new_save && !change.deaths)
                text = L"Melvor has not saved since you hid it, so new progress is not visible yet.";
            else if (text = companion::describe_change(change); text.empty())
                text = L"No new XP or GP in the latest save.";
            notify(L"While you were away · " + companion::duration_text(hidden_for), text);
        } catch (const std::exception& error) {
            companion::log_event(std::string("Away summary: could not read the save: ") + error.what());
        }
    }

    // notify once per hide. death has its own look
    void check_idle(bool hidden_now) {
        idle_now = false;
        if (!hidden_now || !(notify_flags & companion::Preferences::notify_idle)) return;
        activity.refresh();
        idle_now = activity.idle();
        if (idle_now && !idle_warned && !died()) {
            notify(L"Melvor is idle", L"The latest save shows no active skill or action. The game is still hidden.");
            idle_warned = true;
        }
    }

    companion::DeathEvidence saved_deaths() {
        try {
            auto save = companion::read_progress_save(activity.source());
            if (save.activity.save_id == activity.snapshot().save_id && save.saved_at == activity.saved_at())
                return save.death_evidence();
        } catch (const std::exception& error) {
            companion::log_event(std::string("Death check: could not read the save: ") + error.what());
        }
        return {};
    }

    // every new save, hidden or not
    template<class ReadDeaths>
    void observe_save(const companion::ActivitySnapshot& now, SaveTime stamp, const std::filesystem::path& source, ReadDeaths read_deaths) {
        if (death_tracker.select_character(now.save_id)) {
            death_checked_save.reset();
            death_warned = false;
            if (hide_start) death_tracker.begin_hidden(*hide_start);
            if (now.skill.empty() && now.action.empty() && !source.empty()) {
                auto backup = source;
                backup.replace_extension(L".bak");
                try {
                    std::error_code ec;
                    if (std::filesystem::is_regular_file(backup, ec)) {
                        auto previous = companion::read_progress_save(backup);
                        if (companion::DeathTracker::recent_active_backup(previous.activity, now))
                            death_tracker.observe(false, previous.activity.skill == L"Combat", previous.activity.action,
                                stamp, [&] { return previous.death_evidence(); }, previous.activity.skill == L"Thieving");
                    }
                } catch (const std::exception& error) { companion::log_event(std::string("Death check: previous save unavailable: ") + error.what()); }
            }
        }
        if (!death_checked_save || stamp > *death_checked_save) {
            death_checked_save = stamp;
            auto event = death_tracker.observe(now.skill.empty() && now.action.empty(), now.skill == L"Combat",
                                               now.action, stamp, read_deaths, now.skill == L"Thieving");
            if (event == companion::DeathTracker::Event::died) {
                death_warned = false;
                companion::log_event(death_tracker.possible_thieving_death() ? "Possible Thieving death detected." : "Character death detected.");
            } else if (event == companion::DeathTracker::Event::respawned) {
                companion::log_event("Character respawned.");
            }
        }
    }

    void observe_progress(const companion::ProgressSnapshot& save) {
        observe_save(save.activity, save.saved_at, save.source, [&] { return save.death_evidence(); });
    }

    void check_panel() {
        HWND target = observed.size() == 1 && activity.idle() ? observed.begin()->first : nullptr;
        DWORD pid{};
        if (target) GetWindowThreadProcessId(target, &pid);
        if (panel_result.valid()) {
            if (panel_window != target || panel_character != activity.snapshot().save_id || panel_pid != pid)
                panel_window = nullptr;
            if (panel_result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            if (panel_result.get() && panel_window && panel_window == target) {
                death_tracker.observe_death_panel(SaveTime::clock::now());
                companion::log_event("Detected the game death panel.");
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (!target || !pid || (death_tracker.dead() && !death_tracker.possible_thieving_death()) ||
            now < next_panel_check) return;
        next_panel_check = now + std::chrono::seconds(10);
        panel_window = target;
        panel_character = activity.snapshot().save_id;
        panel_pid = pid;
        // PrintWindow can block on the game: one capture at a time
        std::packaged_task<bool()> capture([target, pid] { return companion::window_has_death_banner(target, pid); });
        try {
            auto result = capture.get_future();
            std::thread worker(std::move(capture));
            panel_result = std::move(result);
            worker.detach();
        } catch (const std::system_error&) { companion::log_event("Could not start the death panel check."); }
    }

    void check_death(bool hidden_now) {
        bool warn = (notify_flags & companion::Preferences::notify_death) != 0;
        if (!warn && !(hidden_now && away_baseline)) return;
        activity.refresh();
        if (activity.available())
            observe_save(activity.snapshot(), activity.saved_at(), activity.source(), [this] { return saved_deaths(); });
        check_panel();
        if (died() && hidden_now && warn) {
            if (!death_warned) {
                death_warned = true;
                if (death_tracker.possible_thieving_death()) {
                    notify(L"Possible Thieving death", L"Thieving stopped after enough damage to exhaust your saved HP. Check your character.");
                    return;
                }
                const auto& enemy = death_tracker.enemy();
                notify(L"Your character died",
                       (enemy.empty() ? std::wstring(L"Your character died")
                                      : L"Your character died fighting " + enemy)
                       + L" and is waiting to respawn. Melvor is hidden.");
            }
        }
    }

    void start_update_check() {
        auto repo = companion::update_repo();
        if (!check_updates || repo.empty()) return;
        {
            std::lock_guard guard(update_state->lock);
            if (update_state->running) return;
            update_state->running = true;
        }
        std::thread([state = update_state, window = icon.hWnd, repo] {
            std::optional<companion::Release> release;
            std::string error;
            try { release = companion::fetch_latest_release(repo); }
            catch (const std::exception& failure) { error = failure.what(); }
            {
                std::lock_guard guard(state->lock);
                state->release = std::move(release);
                state->error = std::move(error);
                state->running = false;
            }
            PostMessageW(window, update_event, 0, 0);
        }).detach();
    }

    void finish_update_check() {
        std::optional<companion::Release> release;
        std::string error;
        {
            std::lock_guard guard(update_state->lock);
            release = update_state->release;
            error = update_state->error;
        }
        if (!error.empty()) { companion::log_event("Update check failed: " + error); return; }
        if (!release || !companion::is_newer(release->tag, companion::app_version())) { available_update.reset(); return; }
        available_update = release;
        if (announced_update != release->tag) {
            announced_update = release->tag;
            notify(L"Melvor Companion " + release->tag + L" is available", L"Open the tray menu to view the release.");
        }
    }

    // so deaths are noticed while the game is visible
    void schedule_death_watch() {
        if (notify_flags & companion::Preferences::notify_death) SetTimer(icon.hWnd, death_timer, 5000, nullptr);
        else KillTimer(icon.hWnd, death_timer);
    }

    void schedule_update_checks() {
        if (check_updates && !companion::update_repo().empty()) {
            SetTimer(icon.hWnd, update_timer, update_interval_ms, nullptr);
            start_update_check();
        } else {
            KillTimer(icon.hWnd, update_timer);
            available_update.reset();
        }
    }

    void update_taskbar() {
        if (!(taskbar & 1)) return;
        auto elapsed = hidden.hidden_duration();
        const bool missing = !elapsed && observed.empty();
        companion::TaskbarWidget::Look look;
        if (missing) look.text = L"Game not detected";
        else {
            if (taskbar & 2) {
                std::chrono::hh_mm_ss time(elapsed.value_or(std::chrono::seconds{}));
                look.text += std::format(L"{}:{:02}:{:02}", time.hours().count(), time.minutes().count(), time.seconds().count());
            }
            if (taskbar & 4) look.text += std::format(L" | Today {}h {}m", total.seconds / 3600, total.seconds / 60 % 60);
            if (taskbar & 8) look.text += elapsed ? L" | Hidden" : L" | Windowed";
            if ((taskbar & 16) && !died()) look.text += L" | " + activity.strip();
            if (look.text.starts_with(L" | ")) look.text.erase(0, 3);
        }
        using Art = companion::TaskbarWidget::Art;
        look.art = missing ? Art::missing : died() ? Art::dead : elapsed ? Art::asleep : Art::awake;
        look.alert = idle_now && !died() && elapsed.has_value();
        if (!missing && died()) {
            std::wstring label = death_tracker.possible_thieving_death() ? L"Possible Thieving death" : L"Died";
            look.text = look.text.empty() ? label : label + L" | " + look.text;
        }

        show_icon(!widget.update(look));
    }

    // NIS_HIDDEN, not NIM_DELETE: notifications go through the icon
    void show_icon(bool visible) {
        if (icon_hidden == !visible) return;
        NOTIFYICONDATAW change = icon;
        change.uFlags = NIF_STATE;
        change.dwState = visible ? 0 : NIS_HIDDEN;
        change.dwStateMask = NIS_HIDDEN;
        if (Shell_NotifyIconW(NIM_MODIFY, &change)) icon_hidden = !visible;
    }

    void apply_location() {
        if (taskbar & 1) update_taskbar();
        else {
            widget.remove();
            show_icon(true);
        }
        update_tip();
    }

    bool hide_verified_window(const companion::WindowInfo& target) {
        if ((notify_flags & companion::Preferences::notify_death) && !hidden.owns_window()) {
            // a hidden GPU window can return a blank frame, so look first
            next_panel_check = {};
            check_death(false);
            if (panel_result.valid()) panel_result.wait_for(std::chrono::milliseconds(250));
        }
        return hidden.hide(target);
    }

    void discover() {
        auto result = companion::find_windows(image, window_class);
        for (const auto& target : result.matches) {
            bool& handled = observed[target.handle];
            if (!handled && result.matches.size() == 1 && !result.inaccessible &&
                hidden.recover_previous_hide(target)) {
                handled = true;
                companion::log_event("Recovered game window left hidden by a previous companion.");
                continue;
            }
            if (!IsWindowVisible(target.handle) || handled) continue;
            if (hide_launch && result.matches.size() == 1 && !hidden.owns_window()) {
                if (!hide_verified_window(target)) continue;
                began_hiding();
            }
            handled = true;
        }
        update_tip();
    }


    void account(std::optional<std::chrono::seconds> elapsed, bool flush = false) {
        const auto current = elapsed.value_or(std::chrono::seconds{});
        total.advance(std::time(nullptr), current >= counted ? current - counted : current);
        counted = current;
        if ((flush || !elapsed || total.day != saved.day || total.seconds >= saved.seconds + 60)
            && (total.seconds != saved.seconds || total.day != saved.day)) {
            try {
                prefs.save_total(total);
                saved = total;
            } catch (const std::exception& error) {
                companion::log_event(error.what());
            }
        }
    }

    void select_shortcut(DWORD choice, bool persist) {
        bool enable = !(choice & companion::disabled_bit);
        if (choice == shortcut && enable == (hotkey_id != 0)) {
            if (persist) prefs.save_shortcut(choice);
            return;
        }
        int next = hotkey_id == 1 ? 2 : 1;
        if (enable && !RegisterHotKey(icon.hWnd, next, HIWORD(choice) | MOD_NOREPEAT, LOWORD(choice)))
            throw std::runtime_error("Shortcut unavailable. Choose another shortcut in the tray menu.");
        try {
            if (persist) prefs.save_shortcut(choice);
        } catch (...) {
            if (enable) UnregisterHotKey(icon.hWnd, next);
            throw;
        }
        if (hotkey_id) UnregisterHotKey(icon.hWnd, hotkey_id);
        hotkey_id = enable ? next : 0;
        shortcut = choice;
    }

    void update_tip() {
        if ((taskbar & 16) && !observed.empty()) activity.refresh();
        hidden.refresh_audio();
        const auto elapsed = hidden.hidden_duration();
        account(elapsed);
        check_death(elapsed.has_value()); // before check_idle
        check_idle(elapsed.has_value());
        update_taskbar();
        if (taskbar & 1) widget.track_hover();
        auto tip = tooltip(elapsed, (taskbar & 16) != 0);
        tip.resize((std::min)(tip.size(),size_t(127)));
        HICON desired = state_icon(elapsed.has_value());
        if (tip != icon.szTip || desired != icon.hIcon) {
            auto change = icon;
            change.uFlags = NIF_TIP | NIF_ICON;
            change.hIcon = desired;
            wcscpy_s(change.szTip, tip.c_str());
            if (Shell_NotifyIconW(NIM_MODIFY, &change)) {
                wcscpy_s(icon.szTip, change.szTip);
                icon.hIcon = desired;
            }
        }
        if ((elapsed || (taskbar & 1)) && !timing_session) {
            timing_session = SetTimer(icon.hWnd, tooltip_timer, 1000, nullptr) != 0;
            if (!timing_session) companion::log_event("SetTimer failed for the tray tooltip.");
        } else if (!elapsed && !(taskbar & 1) && timing_session) {
            KillTimer(icon.hWnd, tooltip_timer);
            timing_session = false;
        }
    }

    std::wstring tooltip(std::optional<std::chrono::seconds> elapsed, bool include_activity) const {
        auto text = companion::tray_tooltip(elapsed);
        if (!elapsed && observed.empty()) return text + L"\nGame not detected";
        if (died()) text += death_tracker.possible_thieving_death() ? L"\nPossible Thieving death" : L"\nYour character died";
        if (include_activity && !died()) text += L"\n" + activity.details();
        return text;
    }

    void add_icon(HWND window) {
        icon.cbSize = sizeof(icon);
        icon.hWnd = window;
        icon.uID = 1;
        icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        icon.hIcon = state_icon(hidden.hidden_duration().has_value());
        icon.uCallbackMessage = tray_event;
        auto tip = tooltip(hidden.hidden_duration(), (taskbar & 16) != 0);
        wcsncpy_s(icon.szTip, tip.c_str(), _TRUNCATE);
        if (Shell_NotifyIconW(NIM_ADD, &icon)) icon_hidden = false;
        else if (!Shell_NotifyIconW(NIM_MODIFY, &icon))
            throw std::runtime_error("Shell_NotifyIconW could not register the Melvor Companion tray icon.");
    }

    void execute(UINT command) {
        if(command>=style_teal && command<=style_states) {
            DWORD choice=command-style_teal;
            prefs.save_icon_style(choice);
            icon_style=choice;
            load_icons();
            update_tip();
        }
        if (command == widget_activity) {
            prefs.save_taskbar_options(taskbar ^ 16);
            taskbar ^= 16;
            update_tip();
        }
        if (command == toggle_dark) {
            prefs.save_dark_mode(!companion::dark_mode());
            companion::set_dark_mode(!companion::dark_mode());
            widget.theme_changed();
            update_tip();
        }
        if (command == goals)
            companion::edit_goals(icon.hWnd, prefs.notes(), [this](const auto& text) { prefs.save_notes(text); },
                                  prefs.goals_bounds(), [this](const RECT& bounds) { prefs.save_goals_bounds(bounds); });
        if (command == launch_game) {
            // > 32 means success
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", steam_launch, nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                throw std::runtime_error("Steam could not start Melvor. Check that Steam is installed.");
        }
        if (command == toggle_away || command == toggle_idle || command == toggle_detailed || command == toggle_death) {
            DWORD bit = command == toggle_away ? companion::Preferences::notify_away
                      : command == toggle_idle ? companion::Preferences::notify_idle
                      : command == toggle_detailed ? companion::Preferences::notify_detailed
                                                   : companion::Preferences::notify_death;
            prefs.save_notifications(notify_flags ^ bit);
            notify_flags ^= bit;
            if (!(notify_flags & (companion::Preferences::notify_away | companion::Preferences::notify_detailed)))
                away_baseline.reset();
            if (command == toggle_detailed && (notify_flags & bit) && hidden.owns_window() && !away_baseline)
                capture_baseline();
            if (command == toggle_death) {
                if (!(notify_flags & bit)) death_tracker.clear();
                schedule_death_watch();
            }
            update_tip();
        }
        if (command == toggle_updates) {
            prefs.save_update_check(!check_updates);
            check_updates = !check_updates;
            schedule_update_checks();
        }
        if (command == about_app) show_about(icon.hWnd);
        if (command == open_update && available_update)
            ShellExecuteW(nullptr, L"open", available_update->page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (command >= toggle_taskbar && command <= widget_status) {
            DWORD bit = 1u << (command - toggle_taskbar);
            prefs.save_taskbar_options(taskbar ^ bit);
            taskbar ^= bit;
            apply_location();
        }
        if (command == toggle_startup)
            companion::Preferences::start_with_windows(!companion::Preferences::starts_with_windows());
        if (command == toggle_launch) {
            prefs.save_hide_on_launch(!hide_launch);
            hide_launch = !hide_launch;
        }
        if (command == toggle_keys)
            select_shortcut(hotkey_id ? shortcut | companion::disabled_bit : shortcut & ~companion::disabled_bit, true);
        if (command == configure_key) {
            DWORD previous = shortcut;
            if (hotkey_id) UnregisterHotKey(icon.hWnd, hotkey_id);
            hotkey_id = 0;
            try {
                auto picked = companion::capture_shortcut(icon.hWnd, shortcut & ~companion::disabled_bit);
                select_shortcut(picked ? *picked | (previous & companion::disabled_bit) : previous, picked.has_value());
            } catch (...) {
                select_shortcut(previous, false);
                throw;
            }
        }
        if (command == hide_game) {
            account(hidden.hidden_duration());
            auto result = companion::find_windows(image, window_class);
            if (result.matches.size() != 1 || result.inaccessible || !hide_verified_window(result.matches.front()))
                throw std::runtime_error("Could not hide a unique, visible Melvor window. Open the menu to check its status.");
            companion::log_event("Hid game PID " + std::to_string(result.matches.front().pid));
            began_hiding();
            update_tip();
        }
        if (command == restore_game || command == quit) {
            auto hidden_for = hidden.hidden_duration();
            account(hidden_for, true);
            if (!hidden.restore()) throw std::runtime_error("Windows could not show the game. Try Show game again.");
            const auto shown_at = SaveTime::clock::now();
            if (command == restore_game && hidden_for) report_away(*hidden_for, shown_at);
            hide_start.reset();
            update_tip();
            companion::log_event(command == quit ? "Exit requested; restoration check completed." : "Restore game completed.");
            if (command == quit) PostQuitMessage(0);
        }
    }

    void menu(HWND window) {
        account(hidden.hidden_duration());
        POINT point{}; GetCursorPos(&point);
        bool editing = true;
        while (editing) {
            Menu popup(CreatePopupMenu(), DestroyMenu);
            auto result = companion::find_windows(image, window_class);
            if (available_update) {
                auto label = L"Update available: " + available_update->tag;
                append(popup.get(), MF_STRING, open_update, label.c_str());
                append(popup.get(), MF_SEPARATOR, 0, nullptr);
            }
            if (result.matches.empty() && !hidden.owns_window())
                append(popup.get(), MF_STRING, launch_game, L"Launch Melvor (Steam)");
            append(popup.get(), result.matches.size() == 1 && !hidden.owns_window() ? MF_STRING : MF_GRAYED, 1, L"Hide game");
            append(popup.get(), hidden.owns_window() ? MF_STRING : MF_GRAYED, 2, L"Show game");
            append(popup.get(), MF_STRING, goals, L"Goals...");
            Menu styles(CreatePopupMenu(),DestroyMenu);
            const wchar_t* names[]{L"Soft Teal",L"Ice Blue",L"Lavender",L"State Colors"};
            for(UINT i=0;i<4;++i) append(styles.get(),i==icon_style ? MF_CHECKED : MF_STRING,style_teal+i,names[i]);
            companion::style_menu(styles.get());
            append(popup.get(),MF_POPUP,reinterpret_cast<UINT_PTR>(styles.get()),L"Icon style");
            styles.release();
            append(popup.get(), companion::dark_mode() ? MF_CHECKED : MF_STRING, toggle_dark, L"Dark mode");
            append(popup.get(), MF_SEPARATOR, 0, nullptr);
            auto total_label = std::format(L"Minimized today   {}h {}m", total.seconds / 3600, total.seconds / 60 % 60);
            append(popup.get(), MF_GRAYED, 99, total_label.c_str());
            append(popup.get(), MF_SEPARATOR, 0, nullptr);
            append(popup.get(), hotkey_id ? MF_CHECKED : MF_STRING, 10, L"Enable hotkeys");
            auto key_label = L"Record shortcut   " + companion::chord_label(shortcut);
            append(popup.get(), MF_STRING, 11, key_label.c_str());
            append(popup.get(), MF_SEPARATOR, 0, nullptr);
            append(popup.get(), companion::Preferences::starts_with_windows() ? MF_CHECKED : MF_STRING,
                   toggle_startup, L"Start with Windows");
            append(popup.get(), hide_launch ? MF_CHECKED : MF_STRING, toggle_launch, L"Hide Melvor when it starts");
            Menu notices(CreatePopupMenu(), DestroyMenu);
            append(notices.get(), notify_flags & companion::Preferences::notify_away ? MF_CHECKED : MF_STRING,
                   toggle_away, L"Summary when the game is shown");
            append(notices.get(), notify_flags & companion::Preferences::notify_detailed ? MF_CHECKED : MF_STRING,
                   toggle_detailed, L"Detailed Summary");
            append(notices.get(), notify_flags & companion::Preferences::notify_idle ? MF_CHECKED : MF_STRING,
                   toggle_idle, L"Warn when idle while hidden");
            append(notices.get(), notify_flags & companion::Preferences::notify_death ? MF_CHECKED : MF_STRING,
                   toggle_death, L"Warn when the character dies");
            companion::style_menu(notices.get());
            append(popup.get(), MF_POPUP, reinterpret_cast<UINT_PTR>(notices.get()), L"Notifications");
            notices.release();
            if (!companion::update_repo().empty())
                append(popup.get(), check_updates ? MF_CHECKED : MF_STRING, toggle_updates, L"Check for updates");
            append(popup.get(), MF_SEPARATOR, 0, nullptr);
            append(popup.get(), taskbar & 1 ? MF_CHECKED : MF_STRING, toggle_taskbar, L"Show on taskbar instead of tray");
            Menu widgets(CreatePopupMenu(), DestroyMenu);
            append(widgets.get(), taskbar & 2 ? MF_CHECKED : MF_STRING, widget_session, L"Session time");
            append(widgets.get(), taskbar & 4 ? MF_CHECKED : MF_STRING, widget_today, L"Today's total");
            append(widgets.get(), taskbar & 8 ? MF_CHECKED : MF_STRING, widget_status, L"Game status");
            append(widgets.get(), taskbar & 16 ? MF_CHECKED : MF_STRING, widget_activity, L"Current activity (last save)");
            companion::style_menu(widgets.get());
            append(popup.get(), MF_POPUP, reinterpret_cast<UINT_PTR>(widgets.get()), L"Taskbar widgets");
            widgets.release();
            append(popup.get(), MF_SEPARATOR, 0, nullptr);
            append(popup.get(), MF_STRING, about_app, L"About Melvor Companion");
            append(popup.get(), MF_STRING, 3, L"Exit");
            companion::style_menu(popup.get());
            SetForegroundWindow(window);
            UINT cmd = companion::track_companion_menu(popup.get(), window, point, TPM_RIGHTBUTTON);
            PostMessageW(window, WM_NULL, 0, 0);
            editing = false;
            if (cmd) execute(cmd);
        }
    }
};

void report(HWND window, const char* message) {
    companion::log_event(message);
    OutputDebugStringA(message);
    MessageBoxA(window, message, "Melvor Companion", MB_OK | MB_ICONERROR);
}

LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept
{
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto tray = reinterpret_cast<Tray*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    try {
        if(message==WM_NOTIFYFORMAT) return NFR_UNICODE;
        if (tray && message==WM_NOTIFY) {
            auto* notice=reinterpret_cast<NMHDR*>(lp);
            if(notice->hwndFrom==tray->widget.tooltip() && notice->code==TTN_GETDISPINFOW) {
                if (!tray->observed.empty()) tray->activity.refresh();
                tray->hover_text=tray->tooltip(tray->hidden.hidden_duration(), true);
                auto* info=reinterpret_cast<NMTTDISPINFOW*>(lp);
                info->lpszText=tray->hover_text.data();
                return 0;
            }
        }
        if (tray && (tray->taskbar & 1) && message == WM_SYSCOMMAND && (wp & 0xfff0) == SC_RESTORE) {
            tray->menu(window);
            return 0;
        }
        if (tray && message == game_event) {
            HWND target = reinterpret_cast<HWND>(lp);
            if (wp == EVENT_OBJECT_DESTROY) {
                if (tray->observed.erase(target)) tray->update_tip();
                else tray->update_taskbar();
            }
            else {
                wchar_t name[256]{};
                if (GetClassNameW(target, name, 256) && std::wstring_view(name) == tray->window_class)
                    tray->discover();
            }
            return 0;
        }
        if (message == WM_DRAWITEM && companion::paint_menu_item(window, *reinterpret_cast<DRAWITEMSTRUCT*>(lp))) return TRUE;
        if (message == WM_MEASUREITEM && companion::measure_menu_item(window, *reinterpret_cast<MEASUREITEMSTRUCT*>(lp))) return TRUE;
        if (tray && message == tray->explorer_started && message) {
            tray->add_icon(window);
            companion::log_event("TaskbarCreated: tray icon registered.");
            return 0;
        }
        if (tray && message == already_running_message() && message) {
            if (tray->notify_flags & companion::Preferences::notify_hints)
                tray->notify(L"Melvor Companion is already running",
                             L"Right-click its icon in the notification area for options. It may be under the ^ arrow.");
            return 0;
        }
        if (tray && message == tray_event) {
            if (lp == WM_RBUTTONUP) tray->menu(window);
            if (lp == WM_LBUTTONDBLCLK) tray->execute(tray->hidden.owns_window() ? restore_game : hide_game);
            return 0;
        }
        if (tray && message == WM_TIMER && wp == tooltip_timer) { tray->update_tip(); return 0; }
        if (tray && message == WM_TIMER && wp == update_timer) { tray->start_update_check(); return 0; }
        if (tray && message == WM_TIMER && wp == death_timer) {
            // tooltip timer covers the hidden case
            if (!tray->hidden.owns_window()) tray->update_tip();
            return 0;
        }
        if (tray && message == update_event) { tray->finish_update_check(); return 0; }
        if (tray && message == WM_HOTKEY && tray->hotkey_id && wp == static_cast<WPARAM>(tray->hotkey_id)) {
            tray->execute(tray->hidden.owns_window() ? restore_game : hide_game);
            return 0;
        }
        if (tray && message == WM_CLOSE) { tray->execute(quit); return 0; }
        if (tray && message == WM_COMMAND && !lp) { tray->execute(LOWORD(wp)); return 0; }
        if (tray && message == WM_ENDSESSION && wp) { tray->execute(quit); return 0; }
        if (tray && message == WM_DESTROY) {
            Shell_NotifyIconW(NIM_DELETE, &tray->icon);
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception& error) {
        report(window, error.what());
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}

namespace {
constexpr const wchar_t* version_property = L"MelvorSwitch.Version";
// 0.1.0 -> 256, 0 if unknown
INT_PTR version_code(std::wstring_view version) {
    auto parts = companion::parse_version(version);
    return parts ? ((*parts)[0] << 16) | ((*parts)[1] << 8) | (*parts)[2] : 0;
}

// true if it replaced an older copy and should keep starting
bool hand_off_to_running_copy(const wchar_t* mutex_name) {
    HWND running = FindWindowW(L"MelvorCompanion.TrayHost", mutex_name);
    // pre-0.1.0 builds were titled "Melvor Switch"
    if (!running && std::wstring_view(mutex_name) == L"Local\\MelvorCompanion.Tray.29C100B9")
        running = FindWindowW(L"MelvorCompanion.TrayHost", L"Melvor Switch");
    if (!running) return false;
    DWORD pid{};
    GetWindowThreadProcessId(running, &pid);
    auto running_version = reinterpret_cast<INT_PTR>(GetPropW(running, version_property));
    if (running_version >= version_code(companion::app_version())) {
        AllowSetForegroundWindow(pid);
        PostMessageW(running, already_running_message(), 0, 0);
        return false;
    }
    auto question = L"An older copy of Melvor Companion is already running.\n\nClose it and start version "
                    + companion::app_version() + L" instead? If it is hiding Melvor, the game is shown again first.";
    if (MessageBoxW(nullptr, question.c_str(), L"Melvor Companion", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES)
        return false;
    std::unique_ptr<void, decltype(&CloseHandle)> process(OpenProcess(SYNCHRONIZE, FALSE, pid), CloseHandle);
    // same as Exit: restores the game first
    DWORD_PTR ignored{};
    SendMessageTimeoutW(running, WM_COMMAND, quit, 0, SMTO_ABORTIFHUNG, 5000, &ignored);
    if (process && WaitForSingleObject(process.get(), 5000) == WAIT_OBJECT_0) return true;
    MessageBoxW(nullptr, L"The running copy did not close. Exit it from its tray menu, then start Melvor Companion again.",
                L"Melvor Companion", MB_OK | MB_ICONWARNING);
    return false;
}
}

int companion::run_tray(HINSTANCE instance, const std::filesystem::path& image,
                        const wchar_t* window_class, const wchar_t* mutex_name, const wchar_t* settings_key)
{
    try {
        HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name);
        DWORD mutex_status = GetLastError();
        std::unique_ptr<void, decltype(&CloseHandle)> lock(mutex, CloseHandle);
        if (!lock) throw api_failure("CreateMutexW", mutex_status);
        if (mutex_status == ERROR_ALREADY_EXISTS && !hand_off_to_running_copy(mutex_name)) return 0;
        Tray tray{{}, image.empty() ? melvor_image() : image, window_class, Preferences(settings_key)};
        if(std::wstring_view(settings_key)==L"Software\\Melvor Companion" && Preferences::starts_with_windows())
            Preferences::start_with_windows(true);
        companion::set_dark_mode(tray.prefs.dark_mode());
        tray.load_icons();
        WNDCLASSW wc{};
        wc.lpfnWndProc = procedure;
        wc.hInstance = instance;
        wc.lpszClassName = L"MelvorCompanion.TrayHost";
        if (!RegisterClassW(&wc)) throw api_failure("RegisterClassW", GetLastError());
        // titled with the lock name so a second copy can find this one
        std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)> window(
            CreateWindowW(wc.lpszClassName, mutex_name, WS_POPUP, 0, 0, 0, 0,
                          nullptr, nullptr, instance, &tray), DestroyWindow);
        if (!window) throw api_failure("CreateWindowW", GetLastError());
        SetPropW(window.get(), version_property, reinterpret_cast<HANDLE>(version_code(companion::app_version())));
        tray.widget.set_owner(window.get(), tray_event);
        tray.add_icon(window.get());
        if (!tray.prefs.first_run_done()) {
            if (tray.notify_flags & Preferences::notify_hints)
                tray.notify(L"Melvor Companion is running",
                            L"Find its switch icon in the notification area; it may be under the ^ arrow. "
                            L"Double-click it to hide or show Melvor, or right-click it for options.");
            try { tray.prefs.save_first_run_done(); } catch (const std::exception& error) { companion::log_event(error.what()); }
        }
        if (tray.taskbar & 1) tray.apply_location();
        event_host = window.get();
        std::unique_ptr<std::remove_pointer_t<HWINEVENTHOOK>, decltype(&UnhookWinEvent)> events(
            SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_SHOW, nullptr, game_changed, 0, 0,
                            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS), UnhookWinEvent);
        if (!events) throw api_failure("SetWinEventHook", GetLastError());
        tray.discover();
        tray.schedule_update_checks();
        tray.schedule_death_watch();
        const DWORD preferred = tray.prefs.shortcut();
        try { tray.select_shortcut(preferred, false); }
        catch (const std::exception& error) {
            tray.shortcut = preferred | companion::disabled_bit;
            report(window.get(), error.what());
        }
        log_event("Tray started.");
        MSG msg{};
        for (;;) {
            const auto received = GetMessageW(&msg, nullptr, 0, 0);
            if (received == 0) return 0;
            if (received == -1) throw api_failure("GetMessageW", GetLastError());
            if (companion::summary_message(msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    } catch (const std::exception& error) {
        report(nullptr, error.what());
        return 1;
    }
}







