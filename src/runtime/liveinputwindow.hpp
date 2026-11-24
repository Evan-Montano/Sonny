// liveinputwindow.hpp
// Thread-safe rolling window of the newest one-second records, shared by the quote stream and the prediction loop.

#pragma once

#include "onesecondaggregator.hpp"
#include "predictionanalytics.hpp"
#include "core/inputconfig.hpp"
#include "core/structures.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace Runtime {

    /**
     * @brief Holds consecutive input records followed by a complete held-out evaluation horizon.
     *
     * The stream thread pushes records; the prediction thread copies a snapshot and works on it
     * without holding the lock, so the stream is never blocked by a FAISS query.
     */
    class LiveInputWindow {
    public:
        // MEMBERS

        /** @brief Input seconds plus the largest forecast horizon, retained in one rolling buffer. */
        static constexpr std::size_t CAPACITY = Core::LIVE_INPUT_WINDOW_SECONDS + PredictionEvaluationWindow;

        /**
         * @brief Copy of a full window taken for one prediction cycle.
         */
        struct Snapshot {
            // MEMBERS

            /**
             * @brief Query inputs, oldest to newest. The held-out future is excluded.
             */
            std::vector<Core::MLRecord> Records;

            /**
             * @brief Timestamp of the last input record, immediately before Future.
             */
            std::int64_t LastTimestampMs{};

            /** @brief The immediately following seconds, including timestamps and observation flags. */
            std::array<LiveRecord, PredictionEvaluationWindow> Future{};
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
         * @return Separate input and future records, or nullopt until both portions are full and new.
         */
        std::optional<Snapshot> TakeSnapshotIfUpdated();

    private:
        // MEMBERS

        /**
         * @brief Guards every member below; shared between the stream and prediction threads.
         */
        std::mutex Mutex;

        /**
         * @brief Newest consecutive records, oldest first.
         */
        std::deque<LiveRecord> Records;

        /**
         * @brief Incremented on every change so the prediction loop can skip unchanged windows.
         */
        std::uint64_t Revision = 0;

        /**
         * @brief Revision handed out by the most recent snapshot.
         */
        std::uint64_t SnapshotRevision = 0;
    };

}
