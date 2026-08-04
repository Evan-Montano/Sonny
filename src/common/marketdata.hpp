// marketdata.hpp
// Header file to hold structs and classes related to market data, including chart data and other relevant information.

#pragma once

#include "dtutil.hpp"

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
    UnixTimestamp Timestamp;
    double Open;
    double Close;
    double High;
    double Low;
    std::uint64_t Volume;
};

// Represents a single day's worth of chart data for a single stock or index.
class ChartDay {
public:
    // MEMBERS
    std::string Ticker;    // e.g., "SPY"
    std::string Name;      // e.g., "SPDR S&P 500 ETF Trust"
    std::string Exchange;  // e.g., "NASDAQ"

    Interval Interval;     // e.g., Interval::OneMinute

    std::vector<PriceBar> PriceBars;
};