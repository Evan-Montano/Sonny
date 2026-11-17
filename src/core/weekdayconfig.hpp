// weekdayconfig.hpp
// Shared configuration for the direction-index weekday experiment: which weekdays share a direction
// index, and whether the live runtime searches the all-week indexes, the weekday-group indexes, or both.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace Core {

    /**
     * @brief Which direction indexes the live runtime searches. The magnitude index is always all-week.
     */
    enum class DirectionSearchMode {
        /**
         * @brief Search the all-week direction indexes only (the behaviour before weekday groups).
         */
        AllDays,

        /**
         * @brief Search only the direction indexes of the current New York weekday's group.
         */
        WeekdayGroups,

        /**
         * @brief Run both searches on the same live query and keep their forecasts separate.
         */
        Compare
    };

    constexpr std::string_view DirectionSearchModeName(const DirectionSearchMode mode) {
        switch (mode) {
            case DirectionSearchMode::AllDays: return "AllDays";
            case DirectionSearchMode::WeekdayGroups: return "WeekdayGroups";
            case DirectionSearchMode::Compare: return "Compare";
        }

        return "(invalid mode)";
    }

    constexpr bool SearchesAllDaysDirection(const DirectionSearchMode mode) {
        return mode != DirectionSearchMode::WeekdayGroups;
    }

    constexpr bool SearchesWeekdayDirection(const DirectionSearchMode mode) {
        return mode != DirectionSearchMode::AllDays;
    }

    /**
     * @brief Label of the all-week direction indexes in logs. No weekday group may use it as a name.
     */
    inline constexpr std::string_view ALL_DAYS_DIRECTION_LABEL = "AllDays";

    /**
     * @brief A named set of weekdays whose historical direction vectors share one set of indexes.
     * The name is also the group's directory name, so it must stay stable across rebuilds.
     */
    struct WeekdayGroup {
        std::string_view name;
        std::array<std::chrono::weekday, 7> weekdays{};
        std::size_t weekday_count{};

        constexpr WeekdayGroup(const std::string_view groupName, const std::initializer_list<std::chrono::weekday> groupWeekdays)
            : name(groupName) {
            for (const std::chrono::weekday day : groupWeekdays) {
                if (weekday_count < weekdays.size()) {
                    weekdays[weekday_count] = day;
                }

                ++weekday_count;
            }
        }

        constexpr std::span<const std::chrono::weekday> Weekdays() const {
            return std::span<const std::chrono::weekday>(weekdays.data(), std::min(weekday_count, weekdays.size()));
        }

        constexpr bool Contains(const std::chrono::weekday day) const {
            for (const std::chrono::weekday member : Weekdays()) {
                if (member == day) return true;
            }

            return false;
        }
    };

    // ======================================================================== //
    // EXPERIMENT CONFIGURATION
    // ======================================================================== //

    /**
     * @brief Direction search mode for the live runtime, and the direction indexes "Update Indexes" builds:
     * AllDays builds the all-week direction indexes, WeekdayGroups builds one set per group below,
     * and Compare builds both.
     */
    inline constexpr DirectionSearchMode DIRECTION_SEARCH_MODE = DirectionSearchMode::Compare;

    /**
     * @brief Weekday groups for the weekday direction indexes. Each trading weekday (Monday to Friday)
     * must belong to exactly one group. Regroup by editing this list, for example:
     *
     *     {"Monday",            {std::chrono::Monday}},
     *     {"TuesdayWednesday",  {std::chrono::Tuesday, std::chrono::Wednesday}},
     *     {"ThursdayFriday",    {std::chrono::Thursday, std::chrono::Friday}},
     *
     * Names may use letters, digits, '_' and '-'. Each group's indexes are stored under
     * storage/{indexes,metadata}/<N>s/d/weekdays/<name>/, with a manifest recording the group's
     * weekdays; an index whose manifest no longer matches its group is rebuilt rather than loaded.
     */
    inline constexpr auto DIRECTION_WEEKDAY_GROUPS = std::to_array<WeekdayGroup>({
        {"Monday",    {std::chrono::Monday}},
        {"Tuesday",   {std::chrono::Tuesday}},
        {"Wednesday", {std::chrono::Wednesday}},
        {"Thursday",  {std::chrono::Thursday}},
        {"Friday",    {std::chrono::Friday}},
    });

    // ======================================================================== //
    // VALIDATION AND LOOKUP
    // ======================================================================== //

    /**
     * @brief English name of a weekday, e.g. "Tuesday".
     */
    constexpr std::string_view WeekdayName(const std::chrono::weekday day) {
        constexpr std::array<std::string_view, 7> NAMES{
            "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
        };

        return day.ok() ? NAMES[day.c_encoding()] : std::string_view{"(invalid weekday)"};
    }

    /**
     * @brief Checks that the groups name each trading weekday exactly once, and nothing else.
     * Evaluated at compile time for DIRECTION_WEEKDAY_GROUPS (a bad configuration fails the build at
     * the throw that describes it), and at runtime for groups passed in explicitly.
     *
     * @throws std::invalid_argument Describing the first problem found.
     */
    constexpr void ValidateWeekdayGroups(const std::span<const WeekdayGroup> groups) {
        constexpr std::array<std::chrono::weekday, 5> TRADING_WEEKDAYS{
            std::chrono::Monday, std::chrono::Tuesday, std::chrono::Wednesday, std::chrono::Thursday, std::chrono::Friday
        };

        if (groups.empty()) {
            throw std::invalid_argument("At least one direction weekday group must be configured.");
        }

        // Group that claimed each weekday so far, by c_encoding.
        std::array<std::string_view, 7> owners{};

        for (std::size_t g = 0; g < groups.size(); ++g) {
            const WeekdayGroup& group = groups[g];

            bool validName = group.name.empty() == false;
            for (const char c : group.name) {
                const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
                validName = validName && allowed;
            }

            if (validName == false) {
                throw std::invalid_argument(std::format(
                    "Weekday group name \"{}\" must be non-empty and use only letters, digits, '_' or '-'.", group.name
                ));
            }

            if (group.name == ALL_DAYS_DIRECTION_LABEL) {
                throw std::invalid_argument(std::format("\"{}\" is reserved for the all-week direction indexes.", group.name));
            }

            for (std::size_t other = 0; other < g; ++other) {
                if (groups[other].name == group.name) {
                    throw std::invalid_argument(std::format("Weekday group \"{}\" is configured more than once.", group.name));
                }
            }

            if (group.weekday_count == 0) {
                throw std::invalid_argument(std::format("Weekday group \"{}\" has no weekdays.", group.name));
            }

            if (group.weekday_count > group.weekdays.size()) {
                throw std::invalid_argument(std::format("Weekday group \"{}\" lists more weekdays than a week has.", group.name));
            }

            for (const std::chrono::weekday day : group.Weekdays()) {
                if (day.ok() == false || day == std::chrono::Saturday || day == std::chrono::Sunday) {
                    throw std::invalid_argument(std::format(
                        "Weekday group \"{}\" lists {}, which is not a trading weekday.", group.name, WeekdayName(day)
                    ));
                }

                std::string_view& owner = owners[day.c_encoding()];

                if (owner == group.name) {
                    throw std::invalid_argument(std::format(
                        "Weekday group \"{}\" lists {} more than once.", group.name, WeekdayName(day)
                    ));
                }

                if (owner.empty() == false) {
                    throw std::invalid_argument(std::format(
                        "{} is in both weekday groups \"{}\" and \"{}\"; each weekday must belong to exactly one group.",
                        WeekdayName(day), owner, group.name
                    ));
                }

                owner = group.name;
            }
        }

        for (const std::chrono::weekday day : TRADING_WEEKDAYS) {
            if (owners[day.c_encoding()].empty()) {
                throw std::invalid_argument(std::format(
                    "{} is not in any weekday group; each trading weekday must belong to exactly one group.", WeekdayName(day)
                ));
            }
        }
    }

    static_assert(
        (ValidateWeekdayGroups(DIRECTION_WEEKDAY_GROUPS), true),
        "DIRECTION_WEEKDAY_GROUPS is invalid; the error above names the problem."
    );

    /**
     * @brief The group a weekday belongs to, or nullopt if no group lists it (weekends, for valid groups).
     */
    constexpr std::optional<WeekdayGroup> FindWeekdayGroup(
        const std::chrono::weekday day,
        const std::span<const WeekdayGroup> groups = DIRECTION_WEEKDAY_GROUPS
    ) {
        for (const WeekdayGroup& group : groups) {
            if (group.Contains(day)) return group;
        }

        return std::nullopt;
    }

    /**
     * @brief Weekday of a historical trading day as stored in index metadata ("yyyymmdd").
     *
     * The id is the .rec file's date, which is the New York session date the file covers
     * (Dukascopy::ExportFullDay fetches 09:30-16:00 America/New_York for it). Every window in the
     * file, and the future that follows it, belongs to that session, so the date alone fixes the
     * weekday: no time-zone conversion applies, and the forecast target cannot change it.
     *
     * @throws std::invalid_argument If the id is not a valid date.
     */
    constexpr std::chrono::weekday TradingDayWeekday(const std::array<char, 8>& yyyymmdd) {
        int digits[8]{};

        for (std::size_t i = 0; i < yyyymmdd.size(); ++i) {
            if (yyyymmdd[i] < '0' || yyyymmdd[i] > '9') {
                throw std::invalid_argument("Trading day id must be eight digits (yyyymmdd).");
            }

            digits[i] = yyyymmdd[i] - '0';
        }

        const std::chrono::year_month_day date{
            std::chrono::year{digits[0] * 1000 + digits[1] * 100 + digits[2] * 10 + digits[3]},
            std::chrono::month{static_cast<unsigned>(digits[4] * 10 + digits[5])},
            std::chrono::day{static_cast<unsigned>(digits[6] * 10 + digits[7])}
        };

        if (date.ok() == false) {
            throw std::invalid_argument("Trading day id is not a valid calendar date.");
        }

        return std::chrono::weekday{std::chrono::sys_days{date}};
    }

}
