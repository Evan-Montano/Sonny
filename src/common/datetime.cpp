// dtutil.cpp
// Implementation of the DateTime class.

#include "datetime.hpp"
#include <chrono>

namespace Common {

    DateTime::DateTime(const UnixTimestamp &ts,
                       const bool &newYorkTime) : Timestamp(ts), NewYorkTime(newYorkTime) {
        this->CalculateDateTime();
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
        : Year(year), 
          Month(month), 
          Day(day), 
          Hour(hour), 
          Minute(minute), 
          Second(second),
          NewYorkTime(newYorkTime) {
        this->CalculateTimestamp();
    }


    void DateTime::CalculateDateTime() {
        using namespace std::chrono;

        const sys_seconds tp{seconds{this->Timestamp}};

        const auto *zone = this->NewYorkTime ? 
            locate_zone("America/New_York") : 
            current_zone();

        const zoned_time localTime{zone, tp};
        const auto localTp = localTime.get_local_time();

        const local_days dayPoint = floor<days>(localTp);
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

        const auto *zone = this->NewYorkTime ? 
            locate_zone("America/New_York") : 
            current_zone();

        const local_days dayPoint{
            year{this->Year} / month{this->Month} / day{this->Day}
        };

        const local_seconds localTp =
            dayPoint +
            hours{this->Hour} +
            minutes{this->Minute} +
            seconds{this->Second};

        const zoned_time localTime{zone, localTp};

        const sys_seconds sysTp = localTime.get_sys_time();

        this->Timestamp = static_cast<UnixTimestamp>(
            sysTp.time_since_epoch().count()
        );
    }

    const UnixTimestamp &DateTime::GetTimestamp() const {
        return this->Timestamp;
    }

    void DateTime::SetTimestamp(const UnixTimestamp &ts) {
        this->Timestamp = ts;
        this->CalculateDateTime();
    }


    const std::uint16_t &DateTime::GetYear() const {
        return this->Year;
    }

    void DateTime::SetYear(const std::uint16_t &year) {
        this->Year = year;
        this->CalculateTimestamp();
    }


    const std::uint8_t &DateTime::GetMonth() const {
        return this->Month;
    }

    void DateTime::SetMonth(const std::uint8_t &month) {
        this->Month = month;
        this->CalculateTimestamp();
    }


    const std::uint8_t &DateTime::GetDay() const {
        return this->Day;
    }

    void DateTime::SetDay(const std::uint8_t &day) {
        this->Day = day;
        this->CalculateTimestamp();
    }


    const std::uint8_t &DateTime::GetHour() const {
        return this->Hour;
    }

    void DateTime::SetHour(const std::uint8_t &hour) {
        this->Hour = hour;
        this->CalculateTimestamp();
    }


    const std::uint8_t &DateTime::GetMinute() const {
        return this->Minute;
    }

    void DateTime::SetMinute(const std::uint8_t &minute) {
        this->Minute = minute;
        this->CalculateTimestamp();
    }


    const std::uint8_t &DateTime::GetSecond() const {
        return this->Second;
    }

    void DateTime::SetSecond(const std::uint8_t &second) {
        this->Second = second;
        this->CalculateTimestamp();
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
		using namespace std::chrono;
		const auto now = system_clock::now();
		const auto now_ts = duration_cast<seconds>(now.time_since_epoch()).count();
		return DateTime(static_cast<UnixTimestamp>(now_ts), newYorkTime);
    }

    void DateTime::NextDay() {
        using namespace std::chrono;
        year_month_day day(
            year(this->Year),
            month(this->Month),
            std::chrono::day{this->Day}
        );
        day = sys_days{day} + days{1};

        SetYear(static_cast<int>(day.year()));
        SetMonth(static_cast<unsigned>(day.month()));
        SetDay(static_cast<unsigned>(day.day()));
    }

    bool DateTime::Equal(const DateTime &dt, const bool &dayOnly) const {
        bool res = false;

        if (dayOnly) {
            res = dt.GetYear() == this->Year
            && dt.GetMonth() == this->Month
            && dt.GetDay() == this->Day;
        }
        else {
            res = dt.GetTimestamp() == this->Timestamp;
        }

        return res;
    }

    bool DateTime::operator<(const DateTime &dt) const {
        return this->Timestamp < dt.Timestamp;
    }

    bool DateTime::operator>(const DateTime &dt) const {
        return this->Timestamp > dt.Timestamp;
    }

    bool DateTime::operator<=(const DateTime &dt) const {
        return this->Timestamp <= dt.Timestamp;
    }

    bool DateTime::operator>=(const DateTime &dt) const {
        return this->Timestamp >= dt.Timestamp;
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
