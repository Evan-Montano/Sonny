// transformer.hpp
// Contains method definitions for things pertaining to data manipulation or normalization in bulk.

#pragma once

#include "client/webullstream.hpp"
#include "structures.hpp"

#include <vector>

namespace Core {

    /**
     * @brief Takes in the complete records vector for a full trading day and normalizes it in place.
     * normalized = (value - mean) / StandardDeviation
     * @param records 
     */
    void NormalizeRecordsVector(std::vector<MLRecord> &records);

    /**
     * @brief Takes in two quotes from the webull stream and aggregates them into a MLRecord.
     * 
     * @param previousQuote 
     * @param currentQuote 
     * @return std::optional<MLRecord> 
     */
    std::optional<MLRecord> WebullQuoteToMLRecord(const Client::WebullQuote &previousQuote, const Client::WebullQuote &currentQuote);
    
}