#pragma once
#include "game_activity.hpp"
#include <chrono>
#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace companion {
// Decides from successive saves whether the character is dead.
//
// Melvor 2 leaves a dead character idle until the player clicks Respawn, and the save has no
// "dead" flag. So a death is: the combat death count rising above its value in the latest save
// that showed combat, followed by an idle save. It lasts until a save shows any activity again.
//
// Reading the death count means decoding the whole save, so `read_deaths` is called only for
// saves showing combat, and once when an idle stretch begins. Skilling and plain idling stay cheap.
// Pure logic with no Windows calls, so the desktop-free tests can replay save sequences.
class DeathTracker {
public:
    using Time = std::filesystem::file_time_type;
    enum class Event { none, died, respawned };
    using Id = std::array<unsigned char, 16>;

    // A backup can supply the missing active observation on startup. Melvor may give
    // both files the same modification time when rotating them, so use the header times.
    static bool recent_active_backup(const ActivitySnapshot& before, const ActivitySnapshot& now) {
        constexpr unsigned long long minute = 60ULL * 10'000'000;
        return now.skill.empty() && now.action.empty() && before.save_id == now.save_id &&
               (before.skill == L"Combat" || before.skill == L"Thieving") &&
               before.save_ticks && before.save_ticks < now.save_ticks && now.save_ticks - before.save_ticks <= minute;
    }

    bool select_character(const Id& id) {
        if (character_ == id) return false;
        *this = DeathTracker{};
        character_ = id;
        return true;
    }
    void begin_hidden(Time start) {
        hidden_since_ = start;
        completed_idle_.reset();
        hidden_thieving_ = false;
    }
    // A full save taken at hide time also provides a baseline if combat starts and
    // ends between polls. The enemy is unknown when no combat save was observed.
    void seed_count(std::optional<long long> count) {
        if (!deaths_in_combat_) deaths_in_combat_ = count;
    }

    // Feed each new save once. `combat` is true when the save's activity is combat; `enemy` is
    // what is being fought. `read_deaths()` returns the save's death count (or nullopt).
    template <class ReadDeaths>
    Event observe(bool idle, bool combat, const std::wstring& enemy, Time saved_at, ReadDeaths read_deaths,
                  bool thieving = false) {
        if (!idle) {
            idle_checked_ = false;
            Event event = Event::none;
            if (dead_) {
                completed_idle_ = hidden_idle(saved_at);
                if (hidden_since_) hidden_thieving_ |= thieving_death_;
                dead_ = false;

                event = Event::respawned;
            }
            thieving_before_.reset();
            thieving_death_ = false;
            if (combat || thieving) {
                enemy_ = combat ? enemy : L"";
                auto sample = read_deaths();
                if (thieving) thieving_before_ = sample;
                if (sample.combat) deaths_in_combat_ = sample.combat;
            }
            return event;
        }
        if (idle_checked_ || (!deaths_in_combat_ && !thieving_before_)) return Event::none;
        auto sample = read_deaths();
        if (!sample.combat && !sample.thieving_damage) return Event::none;
        idle_checked_ = true;
        bool confirmed = sample.combat && deaths_in_combat_ && *sample.combat > *deaths_in_combat_;
        // A Thieving death stops the action and refills shared HP. Eating and stopping
        // between saves can mimic that recovery, so this signature is explicitly inferred.
        bool inferred = thieving_before_ && thieving_before_->health && *thieving_before_->health > 0 &&
            sample.health && *sample.health >= *thieving_before_->health &&
            sample.thieving_damage && thieving_before_->thieving_damage &&
            *sample.thieving_damage - *thieving_before_->thieving_damage >= *thieving_before_->health;
        if (!confirmed && !inferred) return Event::none;
        thieving_death_ = inferred && !confirmed;

        dead_ = true;
        if (sample.combat) deaths_in_combat_ = sample.combat;
        died_at_ = saved_at; // the first idle save showing the death: within one save interval of it

        return Event::died;
    }

    bool dead() const { return dead_; }
    // The open death panel confirms the current state even with two idle saves.
    // Only the observation time is known when the companion starts after death.
    void observe_death_panel(Time now) {
        if (!dead_) died_at_ = now;
        dead_ = true;
        idle_checked_ = true;
        enemy_.clear();
        thieving_death_ = false;
    }
    bool possible_thieving_death() const { return thieving_death_; }
    bool hidden_thieving_death() const { return hidden_thieving_ || (dead_ && thieving_death_); }
    void clear() { *this = DeathTracker{}; }
    const std::wstring& enemy() const { return enemy_; }

    std::optional<std::chrono::seconds> hidden_idle(Time end) const {
        if (!hidden_since_) return std::nullopt;
        auto total = completed_idle_;
        if (dead_ && died_at_) {
            auto start = std::max(*hidden_since_, *died_at_);
            if (end >= start)
                total = total.value_or(std::chrono::seconds{}) + std::chrono::duration_cast<std::chrono::seconds>(end - start);
        }
        return total;
    }
private:
    std::optional<Id> character_;
    std::optional<DeathEvidence> thieving_before_;
    bool thieving_death_{}, hidden_thieving_{};
    std::optional<Time> hidden_since_;
    std::optional<std::chrono::seconds> completed_idle_;
    bool dead_{};
    bool idle_checked_{}; // this idle stretch was already checked for a death
    std::optional<long long> deaths_in_combat_;
    std::wstring enemy_;
    std::optional<Time> died_at_;
};
}
