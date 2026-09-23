// engine.hpp
// Class and method definitions for all things pertaining to the building and querying of market futures.

#pragma once

#include "structures.hpp"

#include <faiss/IndexFlat.h>

namespace Core {

    /**
     * @brief This class represents a single faiss index structure with abilities for load/store and APIs for ANN queries.
     * 
     */
    class HalfHourIndex {
    private:
        // MEMBERS

        /**
         * @brief Dimension of index vectors. This is calculated by multiplying the
         * number of parameters in MLRecord struct(3): mid_delta, spread_delta, and volume
         * by the target number of seconds we are using as a context window for
         * market future predictions, which is 30s.
         */
        static constexpr int DIMENSION = 90;

        /**
         * @brief Underlying faiss index.
         * 
         */
        faiss::IndexFlatL2 _index;

    public:
        // CONSTRUCTOR

        /**
         * @brief Construct a new Half Hour Index object.
         * Calling an empty constructor will initialize an empty faiss index.
         */
        HalfHourIndex() {
            this->_index = faiss::IndexFlatL2(DIMENSION);
        }

        /**
         * @brief Construct a new Half Hour Index object.
         * Calling this constructor will find the appropriate, existing
         * index structure on disk in "~/storage/indexes/" and load it in.
         * @param numOfMinutes Number of minutes that have passed since the market open.
         */
        HalfHourIndex(const std::size_t &numOfMinutes) {
            // TODO
        }

        // DESTRUCTOR
        ~HalfHourIndex() = default;

        // METHODS

        /**
         * @brief Takes in a vector of MLRecord structs and adds them to the faiss index.
         * We are assuming that the caller has already identified which vectors belong in this
         * particular time block and are passing them in accordingly.
         * We are also placing them in as-is, so if the data must be normalized, that must occur before this call.
         * @param mlVectors 
         */
        void AddVectorsToIndex(const std::vector<Core::MLRecord> &normalizedMLVectors);
    };

}