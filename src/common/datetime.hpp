// dtutil.hpp
// Utility functions for date and time manipulation, specifically for
// converstion between Unix timestamps and human-readable date-time formats.

#pragma once

#include <cstdint>
#include <string>

namespace Common {
    
    // AM and PM macros
    inline static constexpr std::uint8_t PM (const std::uint8_t &hour) {
        return hour > 12 ? hour : hour + 12;
    }

    inline static constexpr std::uint8_t AM (const std::uint8_t &hour) {
        return hour;
    }

    // Month abbreviation defs
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
    
    // Seconds since the Unix epoch; signed so dates before 1970 remain representable.
    using UnixTimestamp = std::int64_t;

    enum class TimeZone { Local, NewYork, UTC };

    /**
     * @brief Represents a validated calendar date and Unix timestamp in a selected time zone.
     *
     * Calendar years 1..32767 must be representable in UTC and the display zone.
     * Invalid dates and times are rejected; failed setters preserve the previous value.
     */
    class DateTime {
    public:
        // CONSTRUCTOR
        explicit DateTime(const UnixTimestamp &ts = 0,
                          const bool &newYorkTime = false);

        explicit DateTime(UnixTimestamp ts, TimeZone zone);

        explicit DateTime(
            const std::uint16_t &year = 1970,
            const std::uint8_t &month = 1,
            const std::uint8_t &day = 1,
            const std::uint8_t &hour = 0,
            const std::uint8_t &minute = 0,
            const std::uint8_t &second = 0,
            const bool &newYorkTime = false
        );

        DateTime(std::uint16_t year, std::uint8_t month, std::uint8_t day,
                 std::uint8_t hour, std::uint8_t minute, std::uint8_t second, TimeZone zone);

        // METHODS
        [[nodiscard]] const UnixTimestamp &GetTimestamp() const;
        void SetTimestamp(const UnixTimestamp &ts);

        /** @brief Construct from signed milliseconds, flooring negative timestamps to seconds. */
        static DateTime FromTimestampMilliseconds(std::int64_t timestampMs, TimeZone zone = TimeZone::Local);
        /** @brief Full Unix timestamp, including the retained fractional second. */
        [[nodiscard]] std::int64_t GetTimestampMilliseconds() const;
        /** @brief Current wall-clock Unix timestamp in milliseconds. */
        static std::int64_t GetCurrentTimestampMilliseconds();

        [[nodiscard]] const std::uint16_t &GetYear() const;
        void SetYear(const std::uint16_t &year);

        [[nodiscard]] const std::uint8_t &GetMonth() const;
        void SetMonth(const std::uint8_t &month);

        [[nodiscard]] const std::uint8_t &GetDay() const;
        void SetDay(const std::uint8_t &day);

        [[nodiscard]] const std::uint8_t &GetHour() const;
        void SetHour(const std::uint8_t &hour);

        [[nodiscard]] const std::uint8_t &GetMinute() const;
        void SetMinute(const std::uint8_t &minute);

        [[nodiscard]] const std::uint8_t &GetSecond() const;
        void SetSecond(const std::uint8_t &second);

        [[nodiscard]] std::string ToString_TS() const;
        [[nodiscard]] std::string ToString_DT() const;
        [[nodiscard]] std::string ToString_Date() const;
        [[nodiscard]] std::string ToString_Time() const;
        /** @brief Format the instant as UTC ISO 8601, independent of the display time zone. */
        [[nodiscard]] std::string ToString_UTC() const;

        [[nodiscard]] bool IsWeekday() const;

        static DateTime GetCurrentDateTime(const bool& newYorkTime = false);
        static DateTime GetCurrentDateTime(TimeZone zone);

        void NextDay();
        void PreviousDay();
        [[nodiscard]] bool Equal(const DateTime &dt, const bool &dayOnly = false) const;

        // OPERATORS

        [[nodiscard]] bool operator<(const DateTime &dt) const;
        [[nodiscard]] bool operator>(const DateTime &dt) const;
        [[nodiscard]] bool operator<=(const DateTime &dt) const;
        [[nodiscard]] bool operator>=(const DateTime &dt) const;
        [[nodiscard]] long operator-(const DateTime &dt) const;

    private:
        // MEMBERS
        UnixTimestamp Timestamp;
        std::uint16_t Year;
        std::uint8_t Month;
        std::uint8_t Day;
        std::uint8_t Hour;
        std::uint8_t Minute;
        std::uint8_t Second;

        TimeZone Zone;
        std::uint16_t Millisecond{0};

        // METHODS
        void CalculateDateTime();
        void CalculateTimestamp();
        void SetCalendar(std::uint16_t year, std::uint8_t month, std::uint8_t day,
                         std::uint8_t hour, std::uint8_t minute, std::uint8_t second);
    };

    /**
     * @brief Holds the beginning and end of a date-time interval.
     */
    struct DateTimeRange {
        // MEMBERS

        DateTime Begin;
        DateTime End;
    };

}
