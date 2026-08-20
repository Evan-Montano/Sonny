// sonny.cpp
// Main entry point for the sonny project.

#include <iostream>
#include "core/chartdata.hpp"

int main() {
    // std::cout << "Chart Information:" << std::endl;
    Core::Chart data("SPY");

    std::cout << 
        "Name: " + data.Name << std::endl <<
        "Symbol: " + data.Symbol << std::endl <<
        "ID: " + data.TickerID << std::endl << std::endl;

    Common::DateTime yesterday(2026, Common::AUG, 19);

    std::cout << "Getting day: " + yesterday.ToString_DT() << std::endl << std::endl;
    Core::ChartDay day = data.GetDay(yesterday);

    std::cout << "First and last bars received from job:" << std::endl << std::endl;
    std::cout << "Timestamp\tOpen\tClose\tHigh\tLow\tVolume" << std::endl;
    
    Common::PriceBar bar = day.PriceBars[0];
    Common::DateTime startTime(day.PriceBars[0].Timestamp, true);
    Common::DateTime endTime(day.PriceBars[day.PriceBars.size()-1].Timestamp, true);

    std::cout <<
        std::format(
            "{}\t{}\t{}\t{}\t{}\t{}\n",
            bar.Timestamp,
            bar.Open,
            bar.Close,
            bar.High,
            bar.Low,
            bar.Volume
        );
    
    bar = day.PriceBars[day.PriceBars.size()-1];
    std::cout <<
        std::format(
            "{}\t{}\t{}\t{}\t{}\t{}\n",
            bar.Timestamp,
            bar.Open,
            bar.Close,
            bar.High,
            bar.Low,
            bar.Volume
        );

    std::cout << std::endl << "Number of bars returned: " + std::to_string(day.PriceBars.size()) << std::endl;
    std::cout << "Start Date-Time: " + startTime.ToString_DT() << std::endl;
    std::cout << "End Date-Time: " + endTime.ToString_DT() << std::endl;    
    
    std::cout << std::endl << std::endl;

    return 0;
}