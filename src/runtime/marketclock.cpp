// marketclock.cpp
// Implementation of the regular-session clock helpers.

#include "marketclock.hpp"

#include <chrono>

namespace Runtime {

    std::optional<std::size_t> GetSecondsSinceMarketOpen(const std::int64_t timestampMs) {
        using namespace std::chrono;

        constexpr std::int64_t MARKET_OPEN_SECONDS = 9 * 60 * 60 + 30 * 60;

        // Convert to Eastern time so the result stays correct across DST changes.
        const zoned_time<milliseconds> easternTime{
            locate_zone("America/New_York"),
            sys_time<milliseconds>{milliseconds{timestampMs}}
        };

        const local_time<milliseconds> localTime = easternTime.get_local_time();
        const local_days localDay = floor<days>(localTime);
        const weekday dayOfWeek{localDay};

        if (dayOfWeek == Saturday || dayOfWeek == Sunday) {
            return std::nullopt;
        }

        const std::int64_t secondsSinceMidnight = duration_cast<seconds>(localTime - localDay).count();
        const std::int64_t secondsSinceOpen = secondsSinceMidnight - MARKET_OPEN_SECONDS;

        if (secondsSinceOpen < 0) {
            return std::nullopt;
        }

        // After hours: reuse the final session second (see header).
        if (secondsSinceOpen >= static_cast<std::int64_t>(SESSION_SECONDS)) {
            return SESSION_SECONDS - 1;
        }

        return static_cast<std::size_t>(secondsSinceOpen);
    }

    std::optional<std::size_t> GetWindowStartSecond(const std::size_t lastSecond, const std::size_t inputWindowSeconds) {
        if (inputWindowSeconds == 0 || lastSecond + 1 < inputWindowSeconds) {
            return std::nullopt;
        }

        return lastSecond + 1 - inputWindowSeconds;
    }

}
