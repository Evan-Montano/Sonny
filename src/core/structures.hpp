// structures.hpp
// Header file to help with containing miscellaneous structure definitions.

#pragma once

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

}