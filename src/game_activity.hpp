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
    unsigned long long save_ticks{}; // .NET DateTime ticks, excluding the kind bits
    std::wstring label() const;
};
// Thrown when a save's format version is not one this build can read with confidence.
class UnsupportedSaveVersion : public std::runtime_error {
public:
    explicit UnsupportedSaveVersion(int version);
    int version() const { return version_; }
private:
    int version_;
};

// Header only. Strict: rejects unknown save versions (UnsupportedSaveVersion).
ActivitySnapshot parse_activity_header(std::span<const unsigned char> bytes);
ActivitySnapshot read_activity_save(const std::filesystem::path& file);

// Melvor's experience curve. Levels above 120 are not tracked.
constexpr int max_level = 120;
long long xp_for_level(int level);
int level_for_xp(double xp);

// Progress recorded in a whole save: the header totals plus each skill's experience.
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
    std::map<std::wstring, SkillProgress> skills; // keyed by display name, e.g. "Woodcutting"
    std::map<std::wstring, long long> items;      // bank quantities by display name, e.g. "Oak Log"
    std::optional<long long> deaths;              // combat deaths so far (statistic), if the save has it
    std::optional<int> health;                   // shared player HP, also used outside combat
    std::optional<double> thieving_damage;       // cumulative damage across Thieving targets
    DeathEvidence death_evidence() const { return {deaths, health, thieving_damage}; }
    std::filesystem::file_time_type saved_at{};
    std::filesystem::path source;
};

// What changed between two snapshots, already sorted for display (largest first).
struct SkillGain {
    std::wstring name;
    double xp{};           // gained
    int from{}, to{};      // levels
    double xp_to_next{};   // 0 at the level cap
};
struct ItemChange {
    std::wstring name;
    long long change{};
};
struct ProgressChange {
    std::vector<SkillGain> skills;
    std::vector<ItemChange> gained, used;
    long long gp{}, slayer_coins{};
    long long deaths{}; // combat deaths between the two saves
    bool possible_thieving_death{};
    // How long the character sat with nothing to do after dying, when the tray measured it
    // (death seen while hidden, until the game was shown or a save showed activity again).
    std::optional<std::chrono::seconds> idle_after_death;
    int total_level_from{}, total_level_to{};
    std::chrono::seconds between_saves{}; // time between the two saves, for rates
    bool empty() const { return skills.empty() && gained.empty() && used.empty() && !gp && !slayer_coins && !deaths; }
};
ProgressChange compare_progress(const ProgressSnapshot& before, const ProgressSnapshot& after);

// "RawShrimp" -> "Raw Shrimp"; "MelvorBase:OakLog" -> "Oak Log".
std::wstring readable_name(const std::wstring& id);
// 48210 -> "48,210"
std::wstring grouped(long long value);
// The whole save. An unknown version is accepted only if the result passes a self-check
// (skill levels sum to the stored total level); otherwise UnsupportedSaveVersion.
ProgressSnapshot parse_progress(std::span<const unsigned char> bytes);
// The whole decompressed save, for diagnostics.
std::vector<unsigned char> decode_save(const std::filesystem::path& file);
ProgressSnapshot read_progress_save(const std::filesystem::path& file);

// The most recently written local Steam save, if any.
struct SaveFile {
    std::filesystem::path path;
    std::filesystem::file_time_type modified{};
};
std::optional<SaveFile> newest_save();

// A short, human-readable account of what changed between two snapshots, one item per line.
// Returns an empty string when nothing changed.
std::wstring describe_progress(const ProgressSnapshot& before, const ProgressSnapshot& after, size_t max_skills = 4);
// The same text for an already computed change (for example with idle_after_death filled in).
std::wstring describe_change(const ProgressChange& change, size_t max_skills = 4);
// 4512 s -> "1h 15m", 300 s -> "5m", 42 s -> "42s".
std::wstring duration_text(std::chrono::seconds elapsed);
std::wstring death_text(const ProgressChange& change);

class GameActivity {
public:
    void refresh() noexcept;
    std::wstring strip() const;
    std::wstring details() const;
    bool available() const { return available_; }
    // True when the latest save shows no active skill or action.
    bool idle() const { return available_ && snapshot_.skill.empty() && snapshot_.action.empty(); }
    // The latest save's activity and when it was written (valid when available()).
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
    std::wstring strip_error_ = L"Activity unavailable"; // short form for the taskbar
};
}
