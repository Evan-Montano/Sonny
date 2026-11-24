// structures.hpp
// Header file to help with containing miscellaneous structure definitions.

#pragma once

#include <cstdint>
namespace Core {

    /**
     * @brief Definition of a machine learning record.
     * 
     */
    struct MLRecord {
        // MEMBERS

        float MidDelta;
        // float spread_delta;
        // float volume;
    };

    /**
     * @brief Statistical parameters used to normalize MLRecords.
     *
     * Each HalfHourIndex owns its own set of parameters.
     * Standard deviations use population statistics (N, not N-1).
     */
    struct NormalizationParameters {
        // MEMBERS

        std::uint64_t SampleCount{};

        double MidDeltaMean{};
        double MidDeltaStddev{};
    };

}
