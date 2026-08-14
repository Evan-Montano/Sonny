// dtutil.cpp
// Implementation of the DateTime class.

#include "dtutil.hpp"
#include <chrono>

namespace Common {

    DateTime::DateTime(const UnixTimestamp &ts) : Timestamp(ts) {
        this->CalculateDateTime();
    }

    DateTime::DateTime(
            const std::uint16_t &year,
            const std::uint8_t &month,
            const std::uint8_t &day,
            const std::uint8_t &hour,
            const std::uint8_t &minute,
            const std::uint8_t &second
        )
        : Year(year), 
          Month(month), 
          Day(day), 
          Hour(hour), 
          Minute(minute), 
          Second(second) {
        this->CalculateTimestamp();
    }


    void DateTime::CalculateDateTime() {
        using namespace std::chrono;

        const sys_seconds tp{seconds{this->Timestamp}};

        const auto* zone = current_zone();

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

        const sys_days dayPoint{
            year{this->Year} / month{this->Month} / day{this->Day}
        };

        const sys_seconds tp = 
            dayPoint +
            hours{this->Hour} + 
            minutes{this->Minute} +
            seconds{this->Second};
        
        this->Timestamp = static_cast<UnixTimestamp>(
            duration_cast<seconds>(tp.time_since_epoch()).count()
        );
    }

    UnixTimestamp DateTime::GetTimestamp() const {
        return this->Timestamp;
    }

    void DateTime::SetTimestamp(const UnixTimestamp &ts) {
        this->Timestamp = ts;
        this->CalculateDateTime();
    }


    std::uint16_t DateTime::GetYear() const {
        return this->Year;
    }

    void DateTime::SetYear(const std::uint16_t &year) {
        this->Year = year;
        this->CalculateTimestamp();
    }


    std::uint8_t DateTime::GetMonth() const {
        return this->Month;
    }

    void DateTime::SetMonth(const std::uint8_t &month) {
        this->Month = month;
        this->CalculateTimestamp();
    }


    std::uint8_t DateTime::GetDay() const {
        return this->Day;
    }

    void DateTime::SetDay(const std::uint8_t &day) {
        this->Day = day;
        this->CalculateTimestamp();
    }


    std::uint8_t DateTime::GetHour() const {
        return this->Hour;
    }

    void DateTime::SetHour(const std::uint8_t &hour) {
        this->Hour = hour;
        this->CalculateTimestamp();
    }


    std::uint8_t DateTime::GetMinute() const {
        return this->Minute;
    }

    void DateTime::SetMinute(const std::uint8_t &minute) {
        this->Minute = minute;
        this->CalculateTimestamp();
    }


    std::uint8_t DateTime::GetSecond() const {
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
}