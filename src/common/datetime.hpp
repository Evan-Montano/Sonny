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
    
    using UnixTimestamp = std::uint64_t;

    // A date + time class. Members are accessed through get/set methods
    // in order to keep all data in sync in real-time always.
    class DateTime {
    public:
        // CONSTRUCTOR
        explicit DateTime(const UnixTimestamp &ts = 0,
                          const bool &newYorkTime = false);

        explicit DateTime(
            const std::uint16_t &year = 1970,
            const std::uint8_t &month = 1,
            const std::uint8_t &day = 1,
            const std::uint8_t &hour = 0,
            const std::uint8_t &minute = 0,
            const std::uint8_t &second = 0,
            const bool &newYorkTime = false
        );

        // METHODS
        [[nodiscard]] const UnixTimestamp &GetTimestamp() const;
        void SetTimestamp(const UnixTimestamp &ts);

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

        [[nodiscard]] bool IsWeekday() const;

        static DateTime GetCurrentDateTime(const bool& newYorkTime = false);

        // OPERATORS
        void NextDay();
        [[nodiscard]] bool Equal(const DateTime &dt, const bool &dayOnly = false) const;
        [[nodiscard]] bool operator<(const DateTime &dt) const;
        [[nodiscard]] bool operator>(const DateTime &dt) const;
        [[nodiscard]] bool operator<=(const DateTime &dt) const;
        [[nodiscard]] bool operator>=(const DateTime &dt) const;

    private:
        // MEMBERS
        UnixTimestamp Timestamp;
        std::uint16_t Year;
        std::uint8_t Month;
        std::uint8_t Day;
        std::uint8_t Hour;
        std::uint8_t Minute;
        std::uint8_t Second;

        bool NewYorkTime;

        // METHODS
        void CalculateDateTime();
        void CalculateTimestamp();
    };

    struct DateTimeRange {
        DateTime Begin;
        DateTime End;
    };

}