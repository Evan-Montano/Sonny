// historicalrecordstore.hpp
// Loads and caches historical .rec files so a match's future seconds can be looked up.

#pragma once

#include "core/structures.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace Runtime {

    /**
     * @brief Bounded cache of whole-day .rec files, keyed by trading day.
     * Only used from the prediction thread; not thread-safe.
     */
    class HistoricalRecordStore {
    public:
        // METHODS

        /**
         * @brief Get every record for a trading day, loading the file on first use.
         * The returned pointer is valid only until the next call, which may evict the cache.
         *
         * @param tradingDayId Day in "yyyymmdd" form, as stored in index metadata.
         * @return The day's records, or nullptr if the file is missing or malformed (logged).
         */
        const std::vector<Core::MLRecord>* Find(const std::array<char, 8>& tradingDayId);

    private:
        // MEMBERS

        /**
         * @brief Days held before the cache is cleared. One day is roughly 280 KB.
         */
        static constexpr std::size_t MAX_CACHED_DAYS = 128;

        /**
         * @brief Loaded days keyed by "yyyy-mm-dd".
         */
        std::unordered_map<std::string, std::vector<Core::MLRecord>> cache_;
    };

}
