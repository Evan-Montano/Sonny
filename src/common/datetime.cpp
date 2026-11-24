// dtutil.cpp
// Implementation of the DateTime class.

#include "datetime.hpp"
#include <chrono>
#include <format>
#include <stdexcept>

namespace Common {

    namespace {
        using namespace std::chrono;
        constexpr auto FIRST_DAY = sys_days{year{1}/January/1};
        constexpr auto END_DAY = sys_days{year{32767}/December/31} + days{1};
        constexpr auto FIRST_SECOND = duration_cast<seconds>(FIRST_DAY.time_since_epoch()).count();
        constexpr auto END_SECOND = duration_cast<seconds>(END_DAY.time_since_epoch()).count();

        const std::chrono::time_zone* ResolveZone(const TimeZone zone) {
            switch (zone) {
                case TimeZone::NewYork: return std::chrono::locate_zone("America/New_York");
                case TimeZone::UTC: return std::chrono::locate_zone("UTC");
                default: return std::chrono::current_zone();
            }
        }
    }


    DateTime::DateTime(const UnixTimestamp &ts,
                       const bool &newYorkTime)
        : DateTime(ts, newYorkTime ? TimeZone::NewYork : TimeZone::Local) {
    }

    DateTime::DateTime(const UnixTimestamp ts, const TimeZone zone)
        : Timestamp(ts), Zone(zone) {
        CalculateDateTime();
    }

    DateTime::DateTime(
            const std::uint16_t &year,
            const std::uint8_t &month,
            const std::uint8_t &day,
            const std::uint8_t &hour,
            const std::uint8_t &minute,
            const std::uint8_t &second,
            const bool &newYorkTime
        )
        : DateTime(year, month, day, hour, minute, second,
                   newYorkTime ? TimeZone::NewYork : TimeZone::Local) {
    }

    DateTime::DateTime(const std::uint16_t year, const std::uint8_t month, const std::uint8_t day,
                       const std::uint8_t hour, const std::uint8_t minute, const std::uint8_t second,
                       const TimeZone zone)
        : Year(year), Month(month), Day(day), Hour(hour), Minute(minute), Second(second), Zone(zone) {
        CalculateTimestamp();
    }


    void DateTime::CalculateDateTime() {
        using namespace std::chrono;

        // Check before any calendar conversion or zone-offset arithmetic can overflow/wrap.
        if (Timestamp < FIRST_SECOND || Timestamp >= END_SECOND) {
            throw std::out_of_range("DateTime must be within calendar years 1..32767 in UTC.");
        }
        const sys_seconds tp{seconds{this->Timestamp}};

        const auto *zone = ResolveZone(Zone);

        const zoned_time localTime{zone, tp};
        const auto localTp = localTime.get_local_time();

        const local_days dayPoint = floor<days>(localTp);
        if (dayPoint.time_since_epoch() < FIRST_DAY.time_since_epoch() ||
            dayPoint.time_since_epoch() >= END_DAY.time_since_epoch()) {
            throw std::out_of_range("DateTime must be within calendar years 1..32767 in its display zone.");
        }
        const year_month_day ymd{dayPoint};
        const hh_mm_ss timeOfDay{localTp - dayPoint};

        this->Year = static_cast<std::uint16_t>(int(ymd.year()));
        this->Month = static_cast<std::uint8_t>(unsigned(ymd.month()));
        this->Day = static_cast<std::uint8_t>(unsigned(ymd.day()));
        this->Hour = static_cast<std::uint8_t>(timeOfDay.hours().count());
        this->Minute = static_cast<std::uint8_t>(timeOfDay.minutes().count());
        this->Second = static_cast<std::uint8_t>(timeOfDay.seconds().count());
    }

    void DateTime::CalculateTimestamp() {
        using namespace std::chrono;

        const auto *zone = ResolveZone(Zone);

        if (Year < 1 || Year > 32767) {
            throw std::out_of_range("DateTime calendar year must be within 1..32767.");
        }
        const year_month_day date{year{Year}, month{Month}, day{Day}};
        if (!date.ok() || Hour > 23 || Minute > 59 || Second > 59) {
            throw std::invalid_argument("Invalid DateTime calendar date or time.");
        }
        const local_days dayPoint{date};

        const local_seconds localTp =
            dayPoint +
            hours{this->Hour} +
            minutes{this->Minute} +
            seconds{this->Second};

        const zoned_time localTime{zone, localTp};

        const sys_seconds sysTp = localTime.get_sys_time();

        this->Timestamp = sysTp.time_since_epoch().count();
        CalculateDateTime();
    }

    const UnixTimestamp &DateTime::GetTimestamp() const {
        return this->Timestamp;
    }

    void DateTime::SetTimestamp(const UnixTimestamp &ts) {
        *this = DateTime(ts, Zone);
    }


    DateTime DateTime::FromTimestampMilliseconds(const std::int64_t timestampMs, const TimeZone zone) {
        // C++ integer division truncates toward zero; calendar conversion needs floor instead.
        UnixTimestamp seconds = timestampMs / 1000;
        std::int64_t remainder = timestampMs % 1000;
        if (remainder < 0) {
            --seconds;
            remainder += 1000;
        }
        DateTime result(seconds, zone);
        result.Millisecond = static_cast<std::uint16_t>(remainder);
        return result;
    }

    std::int64_t DateTime::GetTimestampMilliseconds() const {
        // The validated calendar range fits comfortably in signed 64-bit milliseconds.
        return Timestamp * 1000 + Millisecond;
    }

    std::int64_t DateTime::GetCurrentTimestampMilliseconds() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    const std::uint16_t &DateTime::GetYear() const {
        return this->Year;
    }

    void DateTime::SetYear(const std::uint16_t &year) {
        SetCalendar(year, Month, Day, Hour, Minute, Second);
    }


    const std::uint8_t &DateTime::GetMonth() const {
        return this->Month;
    }

    void DateTime::SetMonth(const std::uint8_t &month) {
        SetCalendar(Year, month, Day, Hour, Minute, Second);
    }


    const std::uint8_t &DateTime::GetDay() const {
        return this->Day;
    }

    void DateTime::SetDay(const std::uint8_t &day) {
        SetCalendar(Year, Month, day, Hour, Minute, Second);
    }


    const std::uint8_t &DateTime::GetHour() const {
        return this->Hour;
    }

    void DateTime::SetHour(const std::uint8_t &hour) {
        SetCalendar(Year, Month, Day, hour, Minute, Second);
    }


    const std::uint8_t &DateTime::GetMinute() const {
        return this->Minute;
    }

    void DateTime::SetMinute(const std::uint8_t &minute) {
        SetCalendar(Year, Month, Day, Hour, minute, Second);
    }


    const std::uint8_t &DateTime::GetSecond() const {
        return this->Second;
    }

    void DateTime::SetSecond(const std::uint8_t &second) {
        SetCalendar(Year, Month, Day, Hour, Minute, second);
    }

    void DateTime::SetCalendar(const std::uint16_t year, const std::uint8_t month, const std::uint8_t day,
                               const std::uint8_t hour, const std::uint8_t minute, const std::uint8_t second) {
        DateTime replacement(year, month, day, hour, minute, second, Zone);
        replacement.Millisecond = Millisecond;
        *this = replacement;
    }

    std::string DateTime::ToString_TS() const {
        return std::to_string(this->Timestamp);
    }
    
    std::string DateTime::ToString_DT() const {
        return std::format(
            "{:04}-{:02}-{:02} {:02}:{:02}:{:02}",
            this->Year,
            static_cast<int>(this->Month),
            static_cast<int>(this->Day),
            static_cast<int>(this->Hour),
            static_cast<int>(this->Minute),
            static_cast<int>(this->Second)
        );
    }

    std::string DateTime::ToString_Date() const {
		return std::format(
			"{:04}-{:02}-{:02}",
			this->Year,
			static_cast<int>(this->Month),
			static_cast<int>(this->Day)
		);
    }

    std::string DateTime::ToString_Time() const {
		return std::format(
			"{:02}:{:02}:{:02}",
			static_cast<int>(this->Hour),
			static_cast<int>(this->Minute),
			static_cast<int>(this->Second)
		);
    }

    std::string DateTime::ToString_UTC() const {
        return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::sys_seconds{std::chrono::seconds{Timestamp}});
    }

    bool DateTime::IsWeekday() const
    {
        std::chrono::year_month_day date{
            std::chrono::year{Year},
            std::chrono::month{Month},
            std::chrono::day{Day}
        };

        std::chrono::weekday weekday{
            std::chrono::sys_days{date}
        };

        return weekday != std::chrono::Saturday &&
            weekday != std::chrono::Sunday;
    }

    DateTime DateTime::GetCurrentDateTime(const bool& newYorkTime) {
        return GetCurrentDateTime(newYorkTime ? TimeZone::NewYork : TimeZone::Local);
    }

    DateTime DateTime::GetCurrentDateTime(const TimeZone zone) {
        return FromTimestampMilliseconds(GetCurrentTimestampMilliseconds(), zone);
    }

    void DateTime::NextDay() {
        using namespace std::chrono;
        const auto target = sys_days{year{Year}/month{Month}/day{Day}} + days{1};
        if (target < FIRST_DAY || target >= END_DAY) {
            throw std::out_of_range("DateTime calendar day is outside years 1..32767.");
        }
        const year_month_day date{target};
        // Apply the date once, avoiding invalid intermediate month/day combinations.
        SetCalendar(static_cast<std::uint16_t>(int(date.year())),
                    static_cast<std::uint8_t>(unsigned(date.month())),
                    static_cast<std::uint8_t>(unsigned(date.day())), Hour, Minute, Second);
    }

    void DateTime::PreviousDay() {
        using namespace std::chrono;
        const auto target = sys_days{year{Year}/month{Month}/day{Day}} - days{1};
        if (target < FIRST_DAY || target >= END_DAY) {
            throw std::out_of_range("DateTime calendar day is outside years 1..32767.");
        }
        const year_month_day date{target};
        // Apply the date once, avoiding invalid intermediate month/day combinations.
        SetCalendar(static_cast<std::uint16_t>(int(date.year())),
                    static_cast<std::uint8_t>(unsigned(date.month())),
                    static_cast<std::uint8_t>(unsigned(date.day())), Hour, Minute, Second);
    }

    bool DateTime::Equal(const DateTime &dt, const bool &dayOnly) const {
        bool res = false;

        if (dayOnly) {
            res = dt.GetYear() == this->Year
            && dt.GetMonth() == this->Month
            && dt.GetDay() == this->Day;
        }
        else {
            res = dt.Timestamp == Timestamp && dt.Millisecond == Millisecond;
        }

        return res;
    }

    bool DateTime::operator<(const DateTime &dt) const {
        return Timestamp < dt.Timestamp || (Timestamp == dt.Timestamp && Millisecond < dt.Millisecond);
    }

    bool DateTime::operator>(const DateTime &dt) const {
        return dt < *this;
    }

    bool DateTime::operator<=(const DateTime &dt) const {
        return !(dt < *this);
    }

    bool DateTime::operator>=(const DateTime &dt) const {
        return !(*this < dt);
    }

    long DateTime::operator-(const DateTime &dt) const {
        using namespace std::chrono;

        year_month_day date1(
            year(this->Year),
            month(this->Month),
            std::chrono::day{this->Day}
        );

        year_month_day date2(
            year(dt.GetYear()),
            month(dt.GetMonth()),
            std::chrono::day{dt.GetDay()}
        );

        auto duration = sys_days{date1} - sys_days{date2};
        return duration.count();
    }

}
