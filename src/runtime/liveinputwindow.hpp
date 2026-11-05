// liveinputwindow.hpp
// Thread-safe rolling window of the newest one-second records, shared by the quote stream and the prediction loop.

#pragma once

#include "onesecondaggregator.hpp"
#include "core/structures.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace Runtime {

    /**
     * @brief Holds the last Core::MAX_INPUT_WINDOW_SECONDS consecutive LiveRecords.
     *
     * The stream thread pushes records; the prediction thread copies a snapshot and works on it
     * without holding the lock, so the stream is never blocked by a FAISS query.
     */
    class LiveInputWindow {
    public:
        /**
         * @brief Copy of a full window taken for one prediction cycle.
         */
        struct Snapshot {
            /**
             * @brief Consecutive one-second records, oldest to newest.
             */
            std::vector<Core::MLRecord> records;

            /**
             * @brief Timestamp of the newest record. Forecasts are anchored to this second.
             */
            std::int64_t last_timestamp_ms{};
        };

        // METHODS

        /**
         * @brief Append the next second, dropping the oldest once the window is full.
         * If the record does not directly follow the newest one, the window is cleared first so a
         * query never spans a gap.
         *
         * @param record Next record from the aggregator.
         * @return Number of records now held.
         */
        std::size_t Push(const LiveRecord& record);

        /**
         * @brief Discard every record, e.g. after the aggregator loses continuity.
         */
        void Clear();

        /**
         * @brief Copy the window if it is full and has changed since the previous snapshot.
         *
         * @return The snapshot, or nullopt if there is nothing new to predict on.
         */
        std::optional<Snapshot> TakeSnapshotIfUpdated();

    private:
        // MEMBERS

        /**
         * @brief Guards every member below; shared between the stream and prediction threads.
         */
        std::mutex mutex_;

        /**
         * @brief Newest consecutive records, oldest first.
         */
        std::deque<LiveRecord> records_;

        /**
         * @brief Incremented on every change so the prediction loop can skip unchanged windows.
         */
        std::uint64_t revision_ = 0;

        /**
         * @brief Revision handed out by the most recent snapshot.
         */
        std::uint64_t snapshot_revision_ = 0;
    };

}
