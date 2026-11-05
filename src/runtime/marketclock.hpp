// marketclock.hpp
// Maps live quote timestamps onto the regular-session clock that the historical records and indexes use.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Runtime {

    /**
     * @brief Number of session seconds covered by one HalfHourIndex segment.
     */
    inline constexpr std::size_t SECONDS_PER_SEGMENT = 30 * 60;

    /**
     * @brief Length of the regular session (09:30-16:00 ET) in seconds. Record N of a .rec file is second N.
     */
    inline constexpr std::size_t SESSION_SECONDS = 6 * 60 * 60 + 30 * 60;

    /**
     * @brief Converts a Unix timestamp into seconds since the 09:30 ET open.
     *
     * Pre-market and weekend timestamps return nullopt. After-hours timestamps are clamped to the
     * last session second so the 15:30-16:00 index can still be exercised outside market hours.
     * Holidays and early-close days are not detected.
     *
     * @param timestampMs Unix timestamp in milliseconds.
     * @return Seconds since the open, or nullopt when no historical session applies.
     */
    std::optional<std::size_t> GetSecondsSinceMarketOpen(std::int64_t timestampMs);

    /**
     * @brief Returns the session second on which a window ending at lastSecond begins.
     *
     * Historical windows never begin before the open, so a live window that still contains
     * pre-market seconds has nothing comparable to match against and returns nullopt.
     *
     * @param lastSecond Session second of the newest record in the window.
     * @param inputWindowSeconds Number of records in the window.
     * @return The window's first session second, or nullopt if it starts before the open.
     */
    std::optional<std::size_t> GetWindowStartSecond(std::size_t lastSecond, std::size_t inputWindowSeconds);

}
