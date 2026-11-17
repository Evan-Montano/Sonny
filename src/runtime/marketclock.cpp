// marketclock.cpp
// Implementation of the regular-session clock helpers.

#include "marketclock.hpp"

#include <chrono>

namespace Runtime {

    namespace {

        /**
         * @brief Eastern wall-clock time of a Unix timestamp; correct across DST changes.
         */
        std::chrono::local_time<std::chrono::milliseconds> ToEasternTime(const std::int64_t timestampMs) {
            using namespace std::chrono;

            const zoned_time<milliseconds> easternTime{
                locate_zone("America/New_York"),
                sys_time<milliseconds>{milliseconds{timestampMs}}
            };

            return easternTime.get_local_time();
        }

    }

    std::optional<std::size_t> GetSecondsSinceMarketOpen(const std::int64_t timestampMs) {
        using namespace std::chrono;

        constexpr std::int64_t MARKET_OPEN_SECONDS = 9 * 60 * 60 + 30 * 60;

        // Convert to Eastern time so the result stays correct across DST changes.
        const local_time<milliseconds> localTime = ToEasternTime(timestampMs);
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

    std::chrono::weekday GetNewYorkWeekday(const std::int64_t timestampMs) {
        return std::chrono::weekday{std::chrono::floor<std::chrono::days>(ToEasternTime(timestampMs))};
    }

    std::optional<std::size_t> GetWindowStartSecond(const std::size_t lastSecond, const std::size_t inputWindowSeconds) {
        if (inputWindowSeconds == 0 || lastSecond + 1 < inputWindowSeconds) {
            return std::nullopt;
        }

        return lastSecond + 1 - inputWindowSeconds;
    }

}
