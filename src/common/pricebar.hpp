// bardata.hpp

#pragma once

#include "datetime.hpp"

namespace Common {

    /**
     * @brief Represents a single price bar in a chart.
     * 
     */
    struct PriceBar {
        Common::UnixTimestamp Timestamp;
        std::uint32_t Open;
        std::uint32_t Close;
        std::uint32_t High;
        std::uint32_t Low;
        std::uint32_t Volume;
    };
}