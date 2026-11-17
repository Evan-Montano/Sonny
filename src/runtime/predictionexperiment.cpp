// predictionexperiment.cpp
// Implementation of the side-by-side forecast experiment.

#include "predictionexperiment.hpp"
#include "common/datetime.hpp"
#include "common/logger.hpp"
#include "core/halfhourindex.hpp"
#include "marketclock.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>

namespace Runtime {

    namespace {

        std::string FormatTime(const std::int64_t timestampMs) {
            return Common::DateTime(static_cast<Common::UnixTimestamp>(timestampMs / 1000)).ToString_Time();
        }

        std::string ConfigurationLabel(const std::size_t inputWindowSeconds, const std::size_t matchCount, const std::string_view streamLabel) {
            return std::format("{}s input / {}-match cap / {}", inputWindowSeconds, matchCount, streamLabel);
        }

        std::string_view GroupName(const std::optional<Core::WeekdayGroup>& group) {
            return group.has_value() ? group->name : std::string_view{};
        }

        /**
         * @brief Start time of a half-hour segment as "HHMM", e.g. segment 5 is "1200".
         */
        std::string SegmentStartLabel(const std::size_t segment) {
            constexpr std::size_t MARKET_OPEN_MINUTES = 9 * 60 + 30;
            const std::size_t startMinutes = MARKET_OPEN_MINUTES + segment * (SECONDS_PER_SEGMENT / 60);

            return std::format("{:02}{:02}", startMinutes / 60, startMinutes % 60);
        }

    }

    // ========== PREDICTION EXPERIMENT ========== //

    PredictionExperiment::PredictionExperiment(
        std::vector<std::size_t> inputWindows,
        const std::size_t maxLoadedSegments,
        const Core::DirectionSearchMode directionSearchMode,
        const std::span<const Core::WeekdayGroup> weekdayGroups
    )
        : input_windows_(std::move(inputWindows)),
          max_loaded_segments_(maxLoadedSegments),
          direction_search_mode_(directionSearchMode),
          weekday_groups_(weekdayGroups.begin(), weekdayGroups.end()),
          slots_(input_windows_.size()) {
        if (max_loaded_segments_ == 0) {
            throw std::invalid_argument("At least one half-hour segment must be allowed to load.");
        }

        Core::ValidateWeekdayGroups(weekday_groups_);

        if (Core::SearchesAllDaysDirection(direction_search_mode_)) {
            streams_.push_back(DirectionStream::AllDays);
        }

        if (Core::SearchesWeekdayDirection(direction_search_mode_)) {
            streams_.push_back(DirectionStream::WeekdayGroup);
        }

        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (Core::IsSupportedInputWindow(input_windows_[i]) == false) {
                throw std::invalid_argument(std::format("Unsupported input window length: {}", input_windows_[i]));
            }

            slots_[i].input_window_seconds = input_windows_[i];
        }
    }

    const std::vector<std::size_t>& PredictionExperiment::InputWindows() const {
        return input_windows_;
    }

    // === OBSERVATION (stream thread) === //

    void PredictionExperiment::ObserveRecord(const LiveRecord& record) {
        for (WindowSlot& slot : slots_) {
            for (const DirectionStream stream : streams_) {
                for (std::size_t i = 0; i < PREDICTION_MATCH_COUNTS.size(); ++i) {
                    LogReports(
                        Analytics(slot, stream)[i].ObserveRecord(record.timestamp_ms, record.record.mid_delta, record.observed),
                        slot.input_window_seconds,
                        PREDICTION_MATCH_COUNTS[i],
                        stream
                    );
                }
            }
        }
    }

    void PredictionExperiment::FlushIncomplete() {
        for (WindowSlot& slot : slots_) {
            for (const DirectionStream stream : streams_) {
                for (std::size_t i = 0; i < PREDICTION_MATCH_COUNTS.size(); ++i) {
                    LogReports(Analytics(slot, stream)[i].FlushIncomplete(), slot.input_window_seconds, PREDICTION_MATCH_COUNTS[i], stream);
                }
            }
        }
    }

    // === PREDICTION (prediction thread) === //

    void PredictionExperiment::Preload(const std::int64_t timestampMs) {
        // Before the open (and on weekends) the next thing needed is the start of a session.
        const std::size_t currentSegment = GetSecondsSinceMarketOpen(timestampMs).value_or(0) / SECONDS_PER_SEGMENT;
        const std::optional<Core::WeekdayGroup> weekdayGroup = SearchedWeekdayGroupAt(timestampMs);

        // Without a trading weekday there is no group to load yet; the first snapshot loads it.
        if (Core::SearchesWeekdayDirection(direction_search_mode_) && weekdayGroup.has_value() == false) {
            Logger::Info("Preload skipped: no weekday group applies until the next trading weekday.", true);
            return;
        }

        for (WindowSlot& slot : slots_) {
            EnsureIndexLoaded(slot, currentSegment, weekdayGroup);
        }
    }

    std::vector<PredictionExperiment::WindowForecasts> PredictionExperiment::RunSnapshot(const LiveInputWindow::Snapshot& snapshot) {
        const std::optional<std::size_t> lastSecond = GetSecondsSinceMarketOpen(snapshot.last_timestamp_ms);

        // Pre-market and weekend data has no historical session to compare against.
        if (lastSecond.has_value() == false) {
            return {};
        }

        // The live session's New York weekday picks the weekday group, the same way each historical
        // window's session date picked the group it was indexed under. Validated groups cover every
        // trading weekday, and weekends returned above.
        const std::optional<Core::WeekdayGroup> weekdayGroup = SearchedWeekdayGroupAt(snapshot.last_timestamp_ms);

        std::vector<WindowForecasts> rows(slots_.size());

        for (std::size_t i = 0; i < slots_.size(); ++i) {
            WindowSlot& slot = slots_[i];

            // Named even when the window ends up unavailable, so the output says which group it was.
            rows[i].weekday_group_name = GroupName(weekdayGroup);

            // Skipped until the window no longer reaches back into pre-market seconds.
            const std::optional<std::size_t> windowStart = GetWindowStartSecond(*lastSecond, slot.input_window_seconds);
            if (windowStart.has_value() == false) {
                continue;
            }

            // The loaded segments follow the time of day; the window's start says which segment it is in,
            // the same way the indexes file each window under the segment it starts in.
            EnsureIndexLoaded(slot, *windowStart / SECONDS_PER_SEGMENT, weekdayGroup);
            if (slot.index == nullptr || slot.index->LoadedSegmentCount() == 0) {
                continue;
            }

            // One configuration failing (bad file, unexpected data) should not stop the others.
            try {
                rows[i] = RunWindow(slot, snapshot);
            }
            catch (const std::exception& error) {
                Logger::Error(std::format("{}s input prediction failed: {}", slot.input_window_seconds, error.what()), true);
            }
        }

        LogComparison(snapshot.last_timestamp_ms, rows);

        return rows;
    }

    void PredictionExperiment::EnsureIndexLoaded(
        WindowSlot& slot,
        const std::size_t currentSegment,
        const std::optional<Core::WeekdayGroup>& weekdayGroup
    ) {
        // A new weekday group needs its own direction indexes. Release everything first so two
        // groups' indexes are never held together; the segments are then reloaded for the new group.
        if (GroupName(slot.weekday_group) != GroupName(weekdayGroup)) {
            slot.index.reset();
            slot.centered_segment.reset();
            slot.weekday_group = weekdayGroup;
        }

        // Nothing changes within a segment, which also keeps unavailable segments from being retried every cycle.
        if (slot.centered_segment == currentSegment) {
            return;
        }

        slot.centered_segment = currentSegment;

        if (slot.index == nullptr) {
            slot.index = std::make_unique<Core::SessionMultiIndex>(
                slot.input_window_seconds,
                Core::HalfHourMultiIndex::SubIndexSelection::ForSearch(direction_search_mode_, slot.weekday_group)
            );
        }

        const std::vector<std::size_t> previouslyLoaded = slot.index->LoadedSegments();

        // Missing or unreadable segments are logged and skipped by the index itself.
        slot.index->LoadSegments(Core::SessionMultiIndex::SelectSegments(currentSegment, max_loaded_segments_));

        const std::vector<std::size_t> loaded = slot.index->LoadedSegments();

        if (loaded.empty()) {
            Logger::Warning(std::format(
                "No index segments are available for the {}s input window around {}; that configuration is skipped.",
                slot.input_window_seconds,
                SegmentStartLabel(std::min(currentSegment, Core::SessionMultiIndex::SEGMENT_COUNT - 1))
            ));
            return;
        }

        if (loaded != previouslyLoaded) {
            Logger::Info(
                std::format(
                    "{}s input: searching {} of {} half-hour segments, {} to {} (limit {}); direction indexes: {}{}{}.",
                    slot.input_window_seconds,
                    loaded.size(),
                    Core::SessionMultiIndex::SEGMENT_COUNT,
                    SegmentStartLabel(loaded.front()),
                    SegmentStartLabel(loaded.back() + 1),
                    max_loaded_segments_,
                    Core::SearchesAllDaysDirection(direction_search_mode_) ? Core::ALL_DAYS_DIRECTION_LABEL : "",
                    direction_search_mode_ == Core::DirectionSearchMode::Compare ? " + " : "",
                    slot.weekday_group.has_value() ? std::format("WeekdayGroups[{}]", slot.weekday_group->name) : std::string{}
                ),
                true
            );
        }
    }

    PredictionExperiment::WindowForecasts PredictionExperiment::RunWindow(WindowSlot& slot, const LiveInputWindow::Snapshot& snapshot) {
        const std::size_t inputWindowSeconds = slot.input_window_seconds;
        WindowForecasts forecasts{.weekday_group_name = GroupName(slot.weekday_group)};

        const std::span<const Core::MLRecord> queryWindow = Core::LatestInputWindow(
            std::span<const Core::MLRecord>(snapshot.records),
            inputWindowSeconds
        );

        // Each segment splits the raw window into |mid_delta| (normalized) and sign(mid_delta) queries.
        // The results are already the nearest across every segment, ranked separately per sub-index.
        // The same direction query searches the all-week and weekday-group indexes.
        const Core::HalfHourMultiIndex::MultiQueryResult results = slot.index->Query(queryWindow, MAX_QUERY_RESULTS);

        // Magnitude is all-week in every mode, so both direction streams share these matches.
        const std::vector<HistoricalMatch> magnitudeMatches =
            CollectHistoricalMatches(results.MagnitudeResults, inputWindowSeconds, MatchWeight, "magnitude");

        if (magnitudeMatches.empty()) {
            Logger::Warning(std::format(
                "{}s input: not enough historical magnitude matches with a full future.",
                inputWindowSeconds
            ));
            return forecasts;
        }

        for (const DirectionStream stream : streams_) {
            const bool allDays = stream == DirectionStream::AllDays;

            (allDays ? forecasts.all_days : forecasts.weekday_group) = RunDirectionStream(
                slot,
                stream,
                magnitudeMatches,
                allDays ? results.DirectionResults : results.WeekdayDirectionResults,
                snapshot.last_timestamp_ms
            );
        }

        return forecasts;
    }

    PredictionExperiment::ForecastRow PredictionExperiment::RunDirectionStream(
        WindowSlot& slot,
        const DirectionStream stream,
        const std::vector<HistoricalMatch>& magnitudeMatches,
        const std::vector<Core::HalfHourIndex::QueryResult>& directionResults,
        const std::int64_t asOfTimestampMs
    ) {
        const std::size_t inputWindowSeconds = slot.input_window_seconds;
        const std::string streamLabel = StreamLabel(stream, asOfTimestampMs);
        ForecastRow row{};

        const std::vector<HistoricalMatch> directionMatches = CollectHistoricalMatches(
            directionResults,
            inputWindowSeconds,
            DirectionMatchWeight,
            std::format("direction, {}", streamLabel)
        );

        // No fallback to another stream's matches: that would blur the comparison.
        if (directionMatches.empty()) {
            Logger::Warning(std::format(
                "{}s input / {}: not enough historical direction matches with a full future.",
                inputWindowSeconds,
                streamLabel
            ));
            return row;
        }

        // Matches are sorted nearest first, so each match count uses a prefix of the same lists.
        for (std::size_t i = 0; i < PREDICTION_MATCH_COUNTS.size(); ++i) {
            const std::size_t matchCount = PREDICTION_MATCH_COUNTS[i];

            const ForecastSummary summary = BuildForecast(
                std::span<const HistoricalMatch>(magnitudeMatches).first(std::min(matchCount, magnitudeMatches.size())),
                std::span<const HistoricalMatch>(directionMatches).first(std::min(matchCount, directionMatches.size())),
                asOfTimestampMs
            );

            LogForecast(summary, inputWindowSeconds, matchCount, streamLabel);
            LogReports(Analytics(slot, stream)[i].SubmitForecast(summary.forecast), inputWindowSeconds, matchCount, stream);
            row[i] = summary.forecast;
        }

        return row;
    }

    std::optional<Core::WeekdayGroup> PredictionExperiment::WeekdayGroupAt(const std::int64_t timestampMs) const {
        return Core::FindWeekdayGroup(GetNewYorkWeekday(timestampMs), weekday_groups_);
    }

    std::optional<Core::WeekdayGroup> PredictionExperiment::SearchedWeekdayGroupAt(const std::int64_t timestampMs) const {
        return Core::SearchesWeekdayDirection(direction_search_mode_) ? WeekdayGroupAt(timestampMs) : std::nullopt;
    }

    std::string PredictionExperiment::StreamLabel(const DirectionStream stream, const std::int64_t asOfTimestampMs) const {
        if (stream == DirectionStream::AllDays) {
            return std::string(Core::ALL_DAYS_DIRECTION_LABEL);
        }

        const std::optional<Core::WeekdayGroup> group = WeekdayGroupAt(asOfTimestampMs);
        return std::format("WeekdayGroups[{}]", group.has_value() ? group->name : "none");
    }

    std::array<PredictionAnalytics, PREDICTION_MATCH_COUNTS.size()>& PredictionExperiment::Analytics(
        WindowSlot& slot,
        const DirectionStream stream
    ) {
        return stream == DirectionStream::AllDays ? slot.all_days_analytics : slot.weekday_analytics;
    }

    std::vector<HistoricalMatch> PredictionExperiment::CollectHistoricalMatches(
        const std::vector<Core::HalfHourIndex::QueryResult>& results,
        const std::size_t inputWindowSeconds,
        double (*weightFunction)(float),
        const std::string_view label
    ) {
        std::vector<HistoricalMatch> matches{};
        matches.reserve(MAX_PREDICTION_MATCHES);

        std::string distances{};

        // Diagnostics: where the selected matches came from.
        std::array<std::size_t, Core::SessionMultiIndex::SEGMENT_COUNT> segmentOrigins{};
        std::set<std::array<char, 8>> tradingDays{};

        for (const Core::HalfHourIndex::QueryResult& result : results) {
            if (matches.size() >= MAX_PREDICTION_MATCHES) {
                break;
            }

            if (std::isfinite(result.distance) == false || result.distance < 0.0f) {
                continue;
            }

            const std::vector<Core::MLRecord>* day = historical_records_.Find(result.tradingDayId);
            if (day == nullptr) {
                continue;
            }

            // Matches near the session close do not have FORECAST_HORIZON_SECONDS of future left.
            const std::size_t futureStart = Core::HistoricalFutureStart(result.recordIndex, inputWindowSeconds);
            if (futureStart > day->size() || FORECAST_HORIZON_SECONDS > day->size() - futureStart) {
                continue;
            }

            // The future is the raw signed deltas for both sub-indexes; BuildForecast decides
            // whether to read its magnitude or its direction.
            HistoricalMatch match{.weight = weightFunction(result.distance)};
            for (std::size_t second = 0; second < FORECAST_HORIZON_SECONDS; ++second) {
                match.mid_deltas[second] = (*day)[futureStart + second].mid_delta;
            }

            matches.push_back(match);
            distances += std::format("{}{:.4f}", distances.empty() ? "" : ", ", result.distance);

            // Each segment's index holds the windows that start inside it, so the start names the segment.
            ++segmentOrigins[std::min(
                static_cast<std::size_t>(result.recordIndex) / SECONDS_PER_SEGMENT,
                Core::SessionMultiIndex::SEGMENT_COUNT - 1
            )];
            tradingDays.insert(result.tradingDayId);
        }

        Logger::Info(std::format("{}s input ({}): {} usable match distances [{}]", inputWindowSeconds, label, matches.size(), distances));

        std::string origins{};
        for (std::size_t segment = 0; segment < segmentOrigins.size(); ++segment) {
            if (segmentOrigins[segment] > 0) {
                origins += std::format("{}{}:{}", origins.empty() ? "" : " ", SegmentStartLabel(segment), segmentOrigins[segment]);
            }
        }

        Logger::Info(std::format(
            "{}s input ({}): match origins by segment [{}] across {} trading days",
            inputWindowSeconds,
            label,
            origins,
            tradingDays.size()
        ));

        return matches;
    }

    // === LOGGING === //

    void PredictionExperiment::LogReports(
        const std::vector<PredictionEvaluationReport>& reports,
        const std::size_t inputWindowSeconds,
        const std::size_t matchCount,
        const DirectionStream stream
    ) const {
        for (const PredictionEvaluationReport& report : reports) {
            // The group is that of the forecast's own weekday, even if the report completes on another.
            const std::string label = ConfigurationLabel(inputWindowSeconds, matchCount, StreamLabel(stream, report.forecast.as_of_timestamp_ms));

            Logger::Info(
                std::format("{}\n{}", label, PredictionAnalytics::FormatReport(report)),
                true
            );
        }
    }

    void PredictionExperiment::LogForecast(
        const ForecastSummary& summary,
        const std::size_t inputWindowSeconds,
        const std::size_t matchCount,
        const std::string_view streamLabel
    ) {
        std::string message = std::format(
            "{}: magnitude {} matches ({:.2f} effective), direction {} matches ({:.2f} effective)\n  {:<8}{:>14}{:>14}{:>12}",
            ConfigurationLabel(inputWindowSeconds, matchCount, streamLabel),
            summary.magnitude_matches,
            summary.magnitude_effective_matches,
            summary.direction_matches,
            summary.direction_effective_matches,
            "Horizon",
            "Forecast",
            "Magnitude",
            "Agreement"
        );

        for (std::size_t i = 0; i < PredictionEvaluationHorizons.size(); ++i) {
            // Agreement is the direction matches' weighted net vote: +1 all up, -1 all down, 0 split.
            message += std::format(
                "\n  {:<8}{:>14}{:>14}{:>+12.3f}",
                std::format("{}s", PredictionEvaluationHorizons[i]),
                std::format("${:+.5f}", summary.forecast.cumulative_changes[i]),
                std::format("${:.5f}", summary.expected_magnitudes[i]),
                summary.direction_agreement[i]
            );
        }

        Logger::Info(message);
    }

    void PredictionExperiment::LogComparison(const std::int64_t asOfTimestampMs, const std::vector<WindowForecasts>& rows) const {
        std::string comparison = std::format(
            "Paired forecasts @{} (same live snapshot; next {} seconds)",
            FormatTime(asOfTimestampMs),
            FORECAST_HORIZON_SECONDS
        );

        for (std::size_t window = 0; window < rows.size(); ++window) {
            for (const DirectionStream stream : streams_) {
                const ForecastRow& row = stream == DirectionStream::AllDays ? rows[window].all_days : rows[window].weekday_group;
                const std::string streamLabel = StreamLabel(stream, asOfTimestampMs);

                for (std::size_t i = 0; i < PREDICTION_MATCH_COUNTS.size(); ++i) {
                    const std::optional<PredictionForecast>& forecast = row[i];

                    comparison += "\n" + ConfigurationLabel(input_windows_[window], PREDICTION_MATCH_COUNTS[i], streamLabel);

                    if (forecast.has_value() == false) {
                        comparison += " | unavailable";
                        continue;
                    }

                    comparison += std::format(" | {} matches", forecast->historical_matches);

                    for (std::size_t h = 0; h < PredictionEvaluationHorizons.size(); ++h) {
                        comparison += std::format(" | {:>2}s ${:+.5f}", PredictionEvaluationHorizons[h], forecast->cumulative_changes[h]);
                    }
                }
            }
        }

        Logger::Info(comparison, true);
    }

    // ========== END PREDICTION EXPERIMENT ========== //

}
