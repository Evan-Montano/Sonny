// chartdata.hpp
// Header file to hold structs and classes related to market data, including chart data and other relevant information.

#pragma once

#include <vector>
#include "../common/dtutil.hpp"
#include "../client/webullclient.hpp"

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
        // Initializes the chart class. On init, we are loading in
        // all identifying information about this chart. We must assume that
        // the symbol is inputted correctly, otherwise the data may come back wrong
        // and cause undefined behavior. If symbol is not provided or the api fails
        // to identify the stock, we throw an exception.
        Chart(const std::string &symbol);
        
        // MEMBERS
        
        // basic info
        std::string TickerID;
		std::string ExchangeID;
		std::string ExchangeCode;
		std::string Type;
		std::string RegionID;
		std::string RegionCode;
		std::string CurrencyID;
		std::string CurrencyCode;
		std::string Name;
		std::string Symbol;

        // chronological vector of ChartDays
        std::vector<ChartDay> Days;

        // METHODS
        void LoadRange(Common::DateTimeRange &range);

    private:
        // MEMBERS
        Client::WebullClient WBClient;
    };

}