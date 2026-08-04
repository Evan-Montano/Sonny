// dtutil.hpp
// Utility functions for date and time manipulation, specifically for
// converstion between Unix timestamps and human-readable date-time formats.

#pragma once

#include <chrono>
#include <format>

// AM and PM macros
inline static constexpr std::uint8_t PM (std::uint8_t hour) {
    return hour > 12 ? hour : hour + 12;
}

inline static constexpr std::uint8_t AM (std::uint8_t hour) {
    return hour;
}

// Month abbreviation macros
inline static constexpr std::uint8_t JAN = 1;
inline static constexpr std::uint8_t FEB = 2;
inline static constexpr std::uint8_t MAR = 3;
inline static constexpr std::uint8_t APR = 4;
inline static constexpr std::uint8_t MAY = 5;
inline static constexpr std::uint8_t JUN = 6;
inline static constexpr std::uint8_t JUL = 7;
inline static constexpr std::uint8_t AUG = 8;
inline static constexpr std::uint8_t SEP = 9;
inline static constexpr std::uint8_t OCT = 10;
inline static constexpr std::uint8_t NOV = 11;
inline static constexpr std::uint8_t DEC = 12;

using UnixTimestamp = std::uint64_t;

struct DateTime {
    std::uint16_t Year;
    std::uint8_t  Month;
    std::uint8_t  Day;
    std::uint8_t  Hour;
    std::uint8_t  Minute;
    std::uint8_t  Second;

    [[nodiscard]]
    static DateTime FromUnixTimestamp(UnixTimestamp timestamp) {
        using namespace std::chrono;
        
        const sys_seconds tp{seconds{timestamp}};

        const sys_days dayPoint = floor<days>(tp);
        const year_month_day ymd{dayPoint};
        const hh_mm_ss timeOfDay{tp - dayPoint};

        return {
            static_cast<std::uint16_t>(int(ymd.year())),
            static_cast<std::uint8_t>(unsigned(ymd.month())),
            static_cast<std::uint8_t>(unsigned(ymd.day())),
            static_cast<std::uint8_t>(timeOfDay.hours().count()),
            static_cast<std::uint8_t>(timeOfDay.minutes().count()),
            static_cast<std::uint8_t>(timeOfDay.seconds().count())
        };
    }

    [[nodiscard]]
    static DateTime FromUnixTimestamp(const std::string &timestamp) {
        try {
            const UnixTimestamp ts = std::stoull(timestamp);
            return FromUnixTimestamp(ts);
        } catch (const std::invalid_argument& e) {
            throw std::invalid_argument("Invalid timestamp string: " + timestamp);
        } catch (const std::out_of_range& e) {
            throw std::out_of_range("Timestamp string out of range: " + timestamp);
        }
    }

    [[nodiscard]]
    UnixTimestamp ToUnixTimestamp() const
    {
        using namespace std::chrono;

        const sys_days dayPoint{
            year{Year} / month{Month} / day{Day}
        };

        const sys_seconds tp =
            dayPoint +
            hours{Hour} +
            minutes{Minute} +
            seconds{Second};

        return static_cast<UnixTimestamp>(
            duration_cast<seconds>(tp.time_since_epoch()).count());
    }

    [[nodiscard]]
    std::string ToUnixTimestampStr() const
    {
        return std::to_string(this->ToUnixTimestamp());
    }

    [[nodiscard]]
    std::string ToString() const
    {
        return std::format(
            "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
            Year,
            Month,
            Day,
            Hour,
            Minute,
            Second
        );
    }
};