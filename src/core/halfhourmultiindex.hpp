// halfhourmultiindex.hpp
// Holds the class definition for HalfHourMultiIndex and other definitions.

#include "halfhourindex.hpp"

namespace Core {

    /**
     * @brief This is an orchestrating class for derived half hour indexes.
     * Since multiple prediction models are being used, this class handles the
     * implementation.
     */
    class HalfHourMultiIndex {
    private:
        // SUB-INDEX CLASSES

        /**
         * @brief Child class of HalfHourIndex. The information building this
         * index is the ABSOLUTE MAGNITUDE of mid_delta changes in a one-second
         * interval.
         */
        class HalfHourIndex_Magnitude : HalfHourIndex {

        };

        /**
         * @brief Child class of HalfHourIndex. The information building this
         * index is the DIRECTION of mid_delta changes in a one-second
         * interval.
         */
        class HalfHourIndex_Direction : HalfHourIndex {

        };


    public:
        // CONSTRUCTORS

        /**
         * @brief Loads a half-hour index for a specific input-window configuration.
         */
        HalfHourMultiIndex(const std::size_t &numOfSeconds, std::size_t inputWindowSeconds);

    };

}