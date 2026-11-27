#pragma once
#include <chrono>
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace companion {
struct ActivitySnapshot {
    std::wstring character;
    std::wstring skill;
    std::wstring action;
    std::array<unsigned char, 16> save_id{};
    unsigned long long save_ticks{}; // .NET ticks without the kind bits
    std::wstring label() const;
};
class UnsupportedSaveVersion : public std::runtime_error {
public:
    explicit UnsupportedSaveVersion(int version);
    int version() const { return version_; }
private:
    int version_;
};

// header only, rejects unknown versions
ActivitySnapshot parse_activity_header(std::span<const unsigned char> bytes);
ActivitySnapshot read_activity_save(const std::filesystem::path& file);

// capped at 120
constexpr int max_level = 120;
long long xp_for_level(int level);
int level_for_xp(double xp);

struct SkillProgress {
    double xp{};
    int level{};
};
struct DeathEvidence {
    std::optional<long long> combat;
    std::optional<int> health;
    std::optional<double> thieving_damage;
};
struct ProgressSnapshot {
    ActivitySnapshot activity;
    int total_level{};
    long long gp{};
    long long slayer_coins{};
    std::map<std::wstring, SkillProgress> skills;
    std::map<std::wstring, long long> items;
    bool bank_read{};
    std::optional<long long> deaths;
    std::optional<int> health; // shared HP, also outside combat
    std::optional<double> thieving_damage; // cumulative
    DeathEvidence death_evidence() const { return {deaths, health, thieving_damage}; }
    std::filesystem::file_time_type saved_at{};
    std::filesystem::path source;
};

// sorted largest first
struct SkillGain {
    std::wstring name;
    double xp{};
    int from{}, to{};
    double xp_to_next{}; // 0 at the cap
};
struct ItemChange {
    std::wstring name;
    long long change{};
};
struct ProgressChange {
    std::vector<SkillGain> skills;
    std::vector<ItemChange> gained, used;
    long long gp{}, slayer_coins{};
    long long deaths{};
    bool possible_thieving_death{};
    // idle time after dying, if the tray measured it
    std::optional<std::chrono::seconds> idle_after_death;
    int total_level_from{}, total_level_to{};
    std::chrono::seconds between_saves{}; // for rates
    bool empty() const { return skills.empty() && gained.empty() && used.empty() && !gp && !slayer_coins && !deaths; }
};
ProgressChange compare_progress(const ProgressSnapshot& before, const ProgressSnapshot& after);

std::wstring readable_name(const std::wstring& id);
std::wstring grouped(long long value);
// unknown versions must pass the total-level check
ProgressSnapshot parse_progress(std::span<const unsigned char> bytes);
// for diagnostics
std::vector<unsigned char> decode_save(const std::filesystem::path& file);
ProgressSnapshot read_progress_save(const std::filesystem::path& file);

struct SaveFile {
    std::filesystem::path path;
    std::filesystem::file_time_type modified{};
};
std::optional<SaveFile> newest_save();

// one change per line, empty if nothing changed
std::wstring describe_progress(const ProgressSnapshot& before, const ProgressSnapshot& after, size_t max_skills = 4);
std::wstring describe_change(const ProgressChange& change, size_t max_skills = 4);
// 4512 s -> "1h 15m", 42 s -> "42s"
std::wstring duration_text(std::chrono::seconds elapsed);
std::wstring death_text(const ProgressChange& change);

class GameActivity {
public:
    void refresh() noexcept;
    std::wstring strip() const;
    std::wstring details() const;
    bool available() const { return available_; }
    bool idle() const { return available_ && snapshot_.skill.empty() && snapshot_.action.empty(); }
    // valid when available()
    const ActivitySnapshot& snapshot() const { return snapshot_; }
    std::filesystem::file_time_type saved_at() const { return modified_; }
    const std::filesystem::path& source() const { return source_; }
private:
    ActivitySnapshot snapshot_;
    std::filesystem::path source_;
    std::filesystem::file_time_type modified_{};
    std::chrono::steady_clock::time_point next_check_{};
    bool available_{};
    std::wstring error_ = L"Waiting for a local save";
    std::wstring strip_error_ = L"Activity unavailable";
};
}
