// marketclock.cpp
// Implementation of the regular-session clock helpers.

#include "marketclock.hpp"

#include "common/datetime.hpp"

namespace Runtime {

    std::optional<std::size_t> GetSecondsSinceMarketOpen(const std::int64_t timestampMs) {
        constexpr std::int64_t MARKET_OPEN_SECONDS = 9 * 60 * 60 + 30 * 60;

        const Common::DateTime easternTime = Common::DateTime::FromTimestampMilliseconds(
            timestampMs, Common::TimeZone::NewYork
        );
        if (!easternTime.IsWeekday()) {
            return std::nullopt;
        }

        const std::int64_t secondsSinceMidnight =
            easternTime.GetHour() * 3600 + easternTime.GetMinute() * 60 + easternTime.GetSecond();
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
