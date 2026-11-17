// predictionexperiment.hpp
// Runs every (active input window × match count) forecast configuration on the same live snapshot and scores each independently.

#pragma once

#include "core/halfhourindex.hpp"
#include "core/inputconfig.hpp"
#include "core/sessionmultiindex.hpp"
#include "core/weekdayconfig.hpp"
#include "forecast.hpp"
#include "historicalrecordstore.hpp"
#include "liveinputwindow.hpp"
#include "onesecondaggregator.hpp"
#include "predictionanalytics.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
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
     * A window is searched across the half-hour segments nearest the current time of day. The
     * runtime defaults to one segment per window when all input windows are active, and uses
     * Core::MAX_LOADED_SEGMENTS for a single active window. Segments only supply candidates; one
     * forecast per window and match count is built from the nearest matches overall.
     *
     * Direction matches come from the indexes chosen by the direction search mode (see
     * Core::DIRECTION_SEARCH_MODE): the all-week indexes (the AllDays baseline), the indexes of the
     * current New York weekday's group (WeekdayGroups), or both (Compare). In Compare mode both are
     * searched with the same live query and form two independent forecast streams, each with its own
     * analytics; they share the same magnitude matches, and neither falls back on the other.
     *
     * Threading: ObserveRecord is called from the stream thread; Preload, RunSnapshot and FlushIncomplete
     * from the prediction thread. They share only the analytics objects, which lock internally.
     */
    class PredictionExperiment {
    public:
        // MEMBERS

        /**
         * @brief Forecasts from one input window, one entry per match count (nullopt if unavailable).
         */
        using ForecastRow = std::array<std::optional<PredictionForecast>, PREDICTION_MATCH_COUNTS.size()>;

        /**
         * @brief Forecasts from one input window, one row per direction stream.
         */
        struct WindowForecasts {
            /**
             * @brief AllDays baseline: direction matches from the all-week indexes.
             * Empty unless the mode searches them.
             */
            ForecastRow all_days{};

            /**
             * @brief WeekdayGroups experiment: direction matches from the current weekday group's indexes only.
             * Empty unless the mode searches them.
             */
            ForecastRow weekday_group{};

            /**
             * @brief Name of the weekday group searched for weekday_group; empty when none was.
             */
            std::string_view weekday_group_name{};
        };

        // CONSTRUCTOR

        /**
         * @brief Construct an experiment with no indexes loaded yet; they load on Preload or on first use.
         *
         * @param inputWindows Input windows to run, each independently. Defaults to the configured
         * selection (Core::USE_ALL_INPUT_WINDOWS / Core::SELECTED_INPUT_WINDOW_SECONDS).
         * @param maxLoadedSegments Most half-hour segments each window keeps in memory at once. The
         * default is one when all input windows are active, otherwise Core::MAX_LOADED_SEGMENTS.
         * @param directionSearchMode Which direction indexes to search; defaults to Core::DIRECTION_SEARCH_MODE.
         * @param weekdayGroups Weekday groups for the weekday direction indexes; defaults to
         * Core::DIRECTION_WEEKDAY_GROUPS.
         * @throws std::invalid_argument If a window is not in Core::INPUT_WINDOW_CONFIGURATIONS, no
         * segment is allowed to load, or the weekday groups are invalid.
         */
        explicit PredictionExperiment(
            std::vector<std::size_t> inputWindows = Core::ActiveInputWindows(),
            std::size_t maxLoadedSegments = Core::DEFAULT_ACTIVE_MAX_LOADED_SEGMENTS,
            Core::DirectionSearchMode directionSearchMode = Core::DIRECTION_SEARCH_MODE,
            std::span<const Core::WeekdayGroup> weekdayGroups = Core::DIRECTION_WEEKDAY_GROUPS
        );

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
         * @brief Load every window's indexes for the given time of day ahead of the first forecast,
         * so that forecast does not have to wait for them.
         *
         * Blocks until the files are read. Before the open the opening segments are loaded, since
         * those are what a session needs first. Weekday-group indexes are those of the timestamp's
         * New York weekday; on a weekend, modes that search weekday groups load nothing yet.
         *
         * @param timestampMs Unix timestamp in milliseconds, normally the current time.
         */
        void Preload(std::int64_t timestampMs);

        /**
         * @brief Forecast the next FORECAST_HORIZON_SECONDS with every configuration, then log a comparison.
         *
         * When the time of day has moved into another half-hour segment, the loaded segments are
         * first moved along with it (one released, one loaded). When the New York weekday has moved
         * into another weekday group, the window's indexes are released and reloaded for that group.
         *
         * @param snapshot Full live window; every configuration is cut from its newest records.
         * @return One entry per entry in InputWindows(), in the same order. Empty when the snapshot
         * falls outside any historical session (pre-market or weekend).
         */
        std::vector<WindowForecasts> RunSnapshot(const LiveInputWindow::Snapshot& snapshot);

        /**
         * @brief Log forecasts whose evaluation window had not finished when the runtime stopped.
         */
        void FlushIncomplete();

    private:
        /**
         * @brief The two independently scored forecast streams.
         */
        enum class DirectionStream { AllDays, WeekdayGroup };

        /**
         * @brief Everything owned by one input-window configuration.
         */
        struct WindowSlot {
            /**
             * @brief Number of seconds in this configuration's query.
             */
            std::size_t input_window_seconds{};

            /**
             * @brief Segment the loaded set was last chosen around; set even if nothing could be
             * loaded, to avoid retrying every cycle.
             */
            std::optional<std::size_t> centered_segment;

            /**
             * @brief Weekday group whose direction indexes index holds; nullopt in AllDays mode.
             */
            std::optional<Core::WeekdayGroup> weekday_group;

            /**
             * @brief The loaded half-hour segments of this window; nullptr until first needed.
             */
            std::unique_ptr<Core::SessionMultiIndex> index;

            /**
             * @brief One scorer per entry in PREDICTION_MATCH_COUNTS for the AllDays stream.
             */
            std::array<PredictionAnalytics, PREDICTION_MATCH_COUNTS.size()> all_days_analytics;

            /**
             * @brief One scorer per entry in PREDICTION_MATCH_COUNTS for the WeekdayGroups stream.
             */
            std::array<PredictionAnalytics, PREDICTION_MATCH_COUNTS.size()> weekday_analytics;
        };

        // METHODS

        /**
         * @brief Make sure slot.index holds the segments nearest currentSegment, up to the limit, with
         * the direction indexes of weekdayGroup when the mode searches weekday groups.
         * Does nothing while currentSegment and the weekday group are unchanged.
         */
        void EnsureIndexLoaded(WindowSlot& slot, std::size_t currentSegment, const std::optional<Core::WeekdayGroup>& weekdayGroup);

        /**
         * @brief Query one input window and submit a forecast for each match count and direction stream.
         */
        WindowForecasts RunWindow(WindowSlot& slot, const LiveInputWindow::Snapshot& snapshot);

        /**
         * @brief Build, log, and submit one direction stream's forecast for each match count.
         * Returns an empty row when the stream has no usable direction matches.
         */
        ForecastRow RunDirectionStream(
            WindowSlot& slot,
            DirectionStream stream,
            const std::vector<HistoricalMatch>& magnitudeMatches,
            const std::vector<Core::HalfHourIndex::QueryResult>& directionResults,
            std::int64_t asOfTimestampMs
        );

        /**
         * @brief The weekday group of a timestamp's New York weekday, or nullopt (weekends).
         */
        std::optional<Core::WeekdayGroup> WeekdayGroupAt(std::int64_t timestampMs) const;

        /**
         * @brief WeekdayGroupAt when the mode searches weekday groups; otherwise nullopt.
         */
        std::optional<Core::WeekdayGroup> SearchedWeekdayGroupAt(std::int64_t timestampMs) const;

        /**
         * @brief Log label of a direction stream: "AllDays", or "WeekdayGroups[<group>]" for the
         * group of the forecast's New York weekday.
         */
        std::string StreamLabel(DirectionStream stream, std::int64_t asOfTimestampMs) const;

        /**
         * @brief The scorers of one direction stream.
         */
        static std::array<PredictionAnalytics, PREDICTION_MATCH_COUNTS.size()>& Analytics(WindowSlot& slot, DirectionStream stream);

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
         * @brief Log finished evaluation reports for one configuration and direction stream.
         */
        void LogReports(
            const std::vector<PredictionEvaluationReport>& reports,
            std::size_t inputWindowSeconds,
            std::size_t matchCount,
            DirectionStream stream
        ) const;

        /**
         * @brief Log one configuration's combined forecast with its magnitude and direction diagnostics.
         */
        static void LogForecast(
            const ForecastSummary& summary,
            std::size_t inputWindowSeconds,
            std::size_t matchCount,
            std::string_view streamLabel
        );

        /**
         * @brief Log every configuration's forecast for one snapshot in a single table.
         */
        void LogComparison(std::int64_t asOfTimestampMs, const std::vector<WindowForecasts>& rows) const;

        // MEMBERS

        /**
         * @brief The input windows being run. Never changes after construction.
         */
        std::vector<std::size_t> input_windows_;

        /**
         * @brief Most half-hour segments each window keeps in memory at once.
         */
        std::size_t max_loaded_segments_;

        /**
         * @brief Which direction indexes are searched.
         */
        Core::DirectionSearchMode direction_search_mode_;

        /**
         * @brief Weekday groups of the weekday direction indexes; validated on construction.
         */
        std::vector<Core::WeekdayGroup> weekday_groups_;

        /**
         * @brief Direction streams the mode runs, in reporting order (AllDays first).
         */
        std::vector<DirectionStream> streams_;

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
