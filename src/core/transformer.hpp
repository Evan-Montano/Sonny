// transformer.hpp
// Contains method definitions for things pertaining to data manipulation or normalization in bulk.

#include "engine.hpp"
#include <vector>

namespace Core {

    /**
     * @brief Takes in the complete records vector for a full trading day and normalizes it in place.
     * normalized = (value - mean) / StandardDeviation
     * @param records 
     */
    void NormalizeRecordsVector(std::vector<MLRecord> &records);
    
}