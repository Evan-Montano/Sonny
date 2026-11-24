// predictionexperiment.hpp
// Runs every (active input window × match count) forecast configuration on the same live snapshot and scores each independently.

#pragma once

#include "core/halfhourindex.hpp"
#include "core/inputconfig.hpp"
#include "core/sessionmultiindex.hpp"
#include "forecast.hpp"
#include "historicalrecordstore.hpp"
#include "liveinputwindow.hpp"
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
    inline constexpr std::array<std::size_t, 5> PREDICTION_MATCH_COUNTS{
        2, 4, 8, 16, 32
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
     * Each active input window (see Core::ActiveInputWindows) has its own indexes. Every match count
     * is evaluated immediately against the snapshot's separate held-out future. Windows are never
     * mixed: every window is searched, ranked, and forecast on its own.
     *
     * A window is searched across the half-hour segments nearest the current time of day. The
     * runtime defaults to one segment per window when all input windows are active, and uses
     * Core::MAX_LOADED_SEGMENTS for a single active window. Segments only supply candidates; one
     * forecast per window and match count is built from the nearest matches overall.
     *
     * Preload and RunSnapshot run on the prediction thread. The stream thread only fills the
     * rolling buffer; later quotes cannot alter an evaluation's captured inputs or outcomes.
     */
    class PredictionExperiment {
    public:
        // MEMBERS

        /**
         * @brief Forecasts and immediate evaluations from one input window, one entry per match count.
         */
        using EvaluationRow = std::array<std::optional<PredictionEvaluationReport>, PREDICTION_MATCH_COUNTS.size()>;

        // CONSTRUCTOR

        /**
         * @brief Construct an experiment with no indexes loaded yet; they load on Preload or on first use.
         *
         * @param inputWindows Input windows to run, each independently. Defaults to the configured
         * selection (Core::USE_ALL_INPUT_WINDOWS / Core::SELECTED_INPUT_WINDOW_SECONDS).
         * @param maxLoadedSegments Most half-hour segments each window keeps in memory at once. The
         * default is one when all input windows are active, otherwise Core::MAX_LOADED_SEGMENTS.
         * @throws std::invalid_argument If a window is not in Core::INPUT_WINDOW_CONFIGURATIONS or no
         * segment is allowed to load.
         */
        explicit PredictionExperiment(
            std::vector<std::size_t> inputWindows = Core::ActiveInputWindows(),
            std::size_t maxLoadedSegments = Core::DEFAULT_ACTIVE_MAX_LOADED_SEGMENTS
        );

        // METHODS

        /**
         * @brief The input windows this experiment runs, in the order RunSnapshot reports them.
         */
        const std::vector<std::size_t>& InputWindows() const;

        /**
         * @brief Load every window's indexes for the given time of day ahead of the first forecast,
         * so that forecast does not have to wait for them.
         *
         * Blocks until the files are read. Before the open the opening segments are loaded, since
         * those are what a session needs first.
         *
         * @param timestampMs Unix timestamp in milliseconds, normally the current time.
         */
        void Preload(std::int64_t timestampMs);

        /**
         * @brief Forecast from the input portion and immediately evaluate the separate held-out future.
         *
         * When the time of day has moved into another half-hour segment, the loaded segments are
         * first moved along with it (one released, one loaded).
         *
         * @param snapshot Captured input and future; every configuration ends at the same input boundary.
         * @return One entry per entry in InputWindows(), in the same order. Empty when the snapshot
         * falls outside any historical session (pre-market or weekend).
         */
        std::vector<EvaluationRow> RunSnapshot(const LiveInputWindow::Snapshot& snapshot);

    private:
        // TYPES

        /**
         * @brief Everything owned by one input-window configuration.
         */
        struct WindowSlot {
            // MEMBERS

            /**
             * @brief Number of seconds in this configuration's query.
             */
            std::size_t InputWindowSeconds{};

            /**
             * @brief Segment the loaded set was last chosen around; set even if nothing could be
             * loaded, to avoid retrying every cycle.
             */
            std::optional<std::size_t> CenteredSegment;

            /**
             * @brief The loaded half-hour segments of this window; nullptr until first needed.
             */
            std::unique_ptr<Core::SessionMultiIndex> Index;
        };

        // METHODS

        /**
         * @brief Make sure slot.Index holds the segments nearest currentSegment, up to the limit, with
         * both magnitude and direction indexes. Does nothing while currentSegment is unchanged.
         */
        void EnsureIndexLoaded(WindowSlot& slot, std::size_t currentSegment);

        /**
         * @brief Query one input window and evaluate each forecast against the captured future.
         */
        EvaluationRow RunWindow(WindowSlot& slot, const LiveInputWindow::Snapshot& snapshot);

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
         * @brief Log one immediately evaluated configuration.
         */
        static void LogReport(
            const PredictionEvaluationReport& report,
            std::size_t inputWindowSeconds,
            std::size_t matchCount
        );

        /**
         * @brief Log one configuration's combined forecast with its magnitude and direction diagnostics.
         */
        static void LogForecast(
            const ForecastSummary& summary,
            std::size_t inputWindowSeconds,
            std::size_t matchCount
        );

        /**
         * @brief Log every configuration's forecast for one snapshot in a single table.
         */
        void LogComparison(const LiveInputWindow::Snapshot& snapshot, const std::vector<EvaluationRow>& rows) const;

        // MEMBERS

        /**
         * @brief The input windows being run. Never changes after construction.
         */
        std::vector<std::size_t> ConfiguredInputWindows;

        /**
         * @brief Most half-hour segments each window keeps in memory at once.
         */
        std::size_t MaxLoadedSegments;

        /**
         * @brief One slot per entry in ConfiguredInputWindows, in the same order. Never resized after construction.
         */
        std::vector<WindowSlot> Slots;

        /**
         * @brief Source of the futures that follow each historical match.
         */
        HistoricalRecordStore HistoricalRecords;
    };

}
