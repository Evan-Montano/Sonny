// sonny.cpp
// Main entry point for the sonny project.

#include <iostream>
#include "core/chartdata.hpp"

int main() {
    std::cout << "Testing getting spy info:" << std::endl;
    Core::Chart data("SPY");

    std::cout << 
        data.Name << std::endl <<
        data.Symbol << std::endl <<
        data.TickerID << std::endl << std::endl;

    // ~/Sonny$ build/sonny 
    // Testing getting spy info:
    // State Street® SPDR® S&P 500® ETF Trust
    // SPY
    // 913243251

    Common::DateTime fridayLastWeek(2026, Common::AUG, 14);
    data.GetDay(fridayLastWeek);

    return 0;
}

// Data array legend:
// 1785527998   746.56    746.61      746.67      746.47      null    3144    null
// TIMESTAMP    OPEN      CLOSE       HIGH        LOW         TBD     VOL     TBD