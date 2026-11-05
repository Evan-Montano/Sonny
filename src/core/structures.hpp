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
        float mid_delta;
        float spread_delta;
        float volume;
    };

    /**
     * @brief Statistical parameters used to normalize MLRecords.
     *
     * Each HalfHourIndex owns its own set of parameters.
     * Standard deviations use population statistics (N, not N-1).
     */
    struct NormalizationParameters {
        std::uint64_t sample_count{};

        double mid_delta_mean{};
        double mid_delta_stddev{};

        double spread_delta_mean{};
        double spread_delta_stddev{};

        // double volume_mean{};
        // double volume_stddev{};
    };

}
