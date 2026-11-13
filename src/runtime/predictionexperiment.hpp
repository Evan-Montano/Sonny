// predictionexperiment.hpp
// Runs every (active input window × match count) forecast configuration on the same live snapshot and scores each independently.

#pragma once

#include "core/halfhourindex.hpp"
#include "core/inputconfig.hpp"
#include "core/sessionmultiindex.hpp"
#include "forecast.hpp"
#include "historicalrecordstore.hpp"
#include "liveinputwindow.hpp"
#include "onesecondaggregator.hpp"
#include "predictionanalytics.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
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
     * @brief Neighbours requested from each half-hour segment, and kept after the segments'
     * candidates are consolidated. Extra headroom over MAX_PREDICTION_MATCHES covers matches that
     * are skipped because they sit too close to the session close to have a full future.
     */
    inline constexpr std::size_t MAX_QUERY_RESULTS = 128;

    /**
     * @brief Side-by-side comparison of forecast configurations while the engine is being tuned.
     *
     * Each active input window (see Core::ActiveInputWindows) has its own indexes and, for each
     * match count, its own PredictionAnalytics so their accuracy is scored separately. Windows are
     * never mixed: every window is searched, ranked, and forecast on its own.
     *
     * A window is searched across all 13 half-hour segments at once. The segments only supply
     * candidates; one forecast per window and match count is built from the nearest matches overall.
     *
     * Threading: ObserveRecord is called from the stream thread; RunSnapshot and FlushIncomplete
     * from the prediction thread. They share only the analytics objects, which lock internally.
     */
    class PredictionExperiment {
    public:
        // MEMBERS

        /**
         * @brief Forecasts from one input window, one entry per match count (nullopt if unavailable).
         */
        using ForecastRow = std::array<std::optional<PredictionForecast>, PREDICTION_MATCH_COUNTS.size()>;

        // CONSTRUCTOR

        /**
         * @brief Construct an experiment with no indexes loaded yet; they load on first use.
         *
         * @param inputWindows Input windows to run, each independently. Defaults to the configured
         * selection (Core::USE_ALL_INPUT_WINDOWS / Core::SELECTED_INPUT_WINDOW_SECONDS).
         * @throws std::invalid_argument If a window is not in Core::INPUT_WINDOW_CONFIGURATIONS.
         */
        explicit PredictionExperiment(std::vector<std::size_t> inputWindows = Core::ActiveInputWindows());

        // METHODS

        /**
         * @brief The input windows this experiment runs, in the order RunSnapshot reports them.
         */
        const std::vector<std::size_t>& InputWindows() const;

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
         * @return One row per entry in InputWindows(), in the same order. Empty when the snapshot
         * falls outside any historical session (pre-market or weekend).
         */
        std::vector<ForecastRow> RunSnapshot(const LiveInputWindow::Snapshot& snapshot);

        /**
         * @brief Log forecasts whose evaluation window had not finished when the runtime stopped.
         */
        void FlushIncomplete();

    private:
        /**
         * @brief Everything owned by one input-window configuration.
         */
        struct WindowSlot {
            /**
             * @brief Number of seconds in this configuration's query.
             */
            std::size_t input_window_seconds{};

            /**
             * @brief Set once loading has been tried, even if it failed, to avoid retrying every cycle.
             */
            bool load_attempted{false};

            /**
             * @brief Every available half-hour segment of this window, or nullptr if none could be loaded.
             */
            std::unique_ptr<Core::SessionMultiIndex> index;

            /**
             * @brief One scorer per entry in PREDICTION_MATCH_COUNTS.
             */
            std::array<PredictionAnalytics, PREDICTION_MATCH_COUNTS.size()> analytics;
        };

        // METHODS

        /**
         * @brief Load slot.index (all 13 half-hour segments) the first time the slot is used.
         */
        static void EnsureIndexLoaded(WindowSlot& slot);

        /**
         * @brief Query one input window and submit a forecast for each match count.
         */
        ForecastRow RunWindow(WindowSlot& slot, const LiveInputWindow::Snapshot& snapshot);

        /**
         * @brief Turn one sub-index's consolidated results into matches with a full future, nearest first,
         * up to MAX_PREDICTION_MATCHES. Each match's future is the real signed .rec data that followed it.
         *
         * @param results Results from either the magnitude or the direction index, already ranked
         * across every half-hour segment.
         * @param inputWindowSeconds Window length the results were queried with.
         * @param weightFunction Converts a FAISS distance into the match's weight.
         * @param label Sub-index name for logging.
         */
        std::vector<HistoricalMatch> CollectHistoricalMatches(
            const std::vector<Core::HalfHourIndex::QueryResult>& results,
            std::size_t inputWindowSeconds,
            double (*weightFunction)(float),
            std::string_view label
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
         * @brief Log one configuration's combined forecast with its magnitude and direction diagnostics.
         */
        static void LogForecast(const ForecastSummary& summary, std::size_t inputWindowSeconds, std::size_t matchCount);

        /**
         * @brief Log every configuration's forecast for one snapshot in a single table.
         */
        void LogComparison(std::int64_t asOfTimestampMs, const std::vector<ForecastRow>& rows) const;

        // MEMBERS

        /**
         * @brief The input windows being run. Never changes after construction.
         */
        std::vector<std::size_t> input_windows_;

        /**
         * @brief One slot per entry in input_windows_, in the same order. Never resized after construction.
         */
        std::vector<WindowSlot> slots_;

        /**
         * @brief Source of the futures that follow each historical match.
         */
        HistoricalRecordStore historical_records_;
    };

}
