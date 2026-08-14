// marketdata.hpp
// Header file to hold structs and classes related to market data, including chart data and other relevant information.

#pragma once

#include "../common/dtutil.hpp"
#include <vector>

namespace Core {

    enum class Interval {
        OneSecond,
        ThirtySeconds,
        OneMinute,
        TwoMinutes,
        ThreeMinutes,
        FiveMinutes,
        TenMinutes,
        FifteenMinutes,
        ThirtyMinutes,
        OneHour,
        TwoHours,
        FourHours,
        OneDay,
        OneWeek,
        OneMonth,
        OneQuarter,
        OneYear
    };

    // Represents a single price bar in a chart.
    struct PriceBar {
        Common::UnixTimestamp Timestamp;
        double Open;
        double Close;
        double High;
        double Low;
        std::uint64_t Volume;
    };

    // Represents an entire day's price data for a chart.
    struct ChartDay {
        // Common::DateTimeRange Range;
        Interval PriceBarInterval;
        std::vector<PriceBar> PriceBars;
    };

    // Class to represent a chart. Includes metadata and price bar info for the selected day(s).
    class Chart {
    public:
        // CONSTRUCTOR
        Chart(std::string ticker);
        
        // MEMBERS
        std::string Ticker;
        std::string TickerID;
        std::string Name;
        std::string Exchange;
        std::vector<ChartDay> Days;

        // METHODS
        // void GetDays(Common::DateTimeRange &range);
    };

}