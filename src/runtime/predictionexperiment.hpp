// predictionexperiment.hpp
// Runs every (input window × match count) forecast configuration on the same live snapshot and scores each independently.

#pragma once

#include "core/halfhourindex.hpp"
#include "core/inputconfig.hpp"
#include "forecast.hpp"
#include "historicalrecordstore.hpp"
#include "liveinputwindow.hpp"
#include "onesecondaggregator.hpp"
#include "predictionanalytics.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace Runtime {

    /**
     * @brief Match counts compared for every input window. Each uses the N nearest matches.
     */
    inline constexpr std::array<std::size_t, 3> PREDICTION_MATCH_COUNTS{
        8, 16, 32
    };

    /**
     * @brief Largest match count; this many usable matches are collected per query.
     */
    inline constexpr std::size_t MAX_PREDICTION_MATCHES = PREDICTION_MATCH_COUNTS.back();

    /**
     * @brief Neighbours requested from FAISS. Extra headroom covers matches that are skipped
     * because they sit too close to the session close to have a full future.
     */
    inline constexpr std::size_t MAX_QUERY_RESULTS = 128;

    /**
     * @brief Side-by-side comparison of forecast configurations while the engine is being tuned.
     *
     * Each input window in Core::INPUT_WINDOW_CONFIGURATIONS has its own index and, for each
     * match count, its own PredictionAnalytics so their accuracy is scored separately.
     *
     * Threading: ObserveRecord is called from the stream thread; RunSnapshot and FlushIncomplete
     * from the prediction thread. They share only the analytics objects, which lock internally.
     */
    class PredictionExperiment {
    public:
        // CONSTRUCTOR

        /**
         * @brief Construct an experiment with no indexes loaded yet; they load on first use.
         */
        PredictionExperiment();

        // METHODS

        /**
         * @brief Record what actually happened in a live second, completing any forecasts it finishes.
         *
         * @param record Next live second, in order.
         */
        void ObserveRecord(const LiveRecord& record);

        /**
         * @brief Forecast the next FORECAST_HORIZON_SECONDS with every configuration, then log a comparison.
         *
         * @param snapshot Full live window; every configuration is cut from its newest records.
         */
        void RunSnapshot(const LiveInputWindow::Snapshot& snapshot);

        /**
         * @brief Log forecasts whose evaluation window had not finished when the runtime stopped.
         */
        void FlushIncomplete();

    private:
        /**
         * @brief Forecasts from one input window, one entry per match count (nullopt if unavailable).
         */
        using ForecastRow = std::array<std::optional<PredictionForecast>, PREDICTION_MATCH_COUNTS.size()>;

        /**
         * @brief Everything owned by one input-window configuration.
         */
        struct WindowSlot {
            /**
             * @brief Number of seconds in this configuration's query.
             */
            std::size_t input_window_seconds{};

            /**
             * @brief Segment the index below was loaded for; set even if loading failed, to avoid retrying every cycle.
             */
            std::optional<std::size_t> loaded_segment;

            /**
             * @brief Index for loaded_segment, or nullptr if it is unavailable.
             */
            std::unique_ptr<Core::HalfHourIndex> index;

            /**
             * @brief One scorer per entry in PREDICTION_MATCH_COUNTS.
             */
            std::array<PredictionAnalytics, PREDICTION_MATCH_COUNTS.size()> analytics;
        };

        // METHODS

        /**
         * @brief Make sure slot.index holds the given segment's index, loading it if needed.
         */
        void EnsureIndexLoaded(WindowSlot& slot, std::size_t segment);

        /**
         * @brief Query one input window and submit a forecast for each match count.
         */
        ForecastRow RunWindow(WindowSlot& slot, const LiveInputWindow::Snapshot& snapshot);

        /**
         * @brief Turn FAISS results into matches with a full future, nearest first, up to MAX_PREDICTION_MATCHES.
         */
        std::vector<HistoricalMatch> CollectHistoricalMatches(
            const std::vector<Core::HalfHourIndex::QueryResult>& results,
            std::size_t inputWindowSeconds
        );

        /**
         * @brief Log finished evaluation reports for one configuration.
         */
        static void LogReports(
            const std::vector<PredictionEvaluationReport>& reports,
            std::size_t inputWindowSeconds,
            std::size_t matchCount
        );

        /**
         * @brief Log one configuration's forecast with its spread and agreement diagnostics.
         */
        static void LogForecast(const ForecastSummary& summary, std::size_t inputWindowSeconds, std::size_t matchCount);

        /**
         * @brief Log every configuration's forecast for one snapshot in a single table.
         */
        static void LogComparison(
            std::int64_t asOfTimestampMs,
            const std::array<ForecastRow, Core::INPUT_WINDOW_CONFIGURATIONS.size()>& rows
        );

        // MEMBERS

        /**
         * @brief One slot per entry in Core::INPUT_WINDOW_CONFIGURATIONS, in the same order.
         */
        std::array<WindowSlot, Core::INPUT_WINDOW_CONFIGURATIONS.size()> slots_{};

        /**
         * @brief Source of the futures that follow each historical match.
         */
        HistoricalRecordStore historical_records_;
    };

}
