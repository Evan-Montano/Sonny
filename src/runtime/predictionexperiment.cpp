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
#include <span>

namespace Runtime {

    namespace {

        std::string FormatTime(const std::int64_t timestampMs) {
            return Common::DateTime(static_cast<Common::UnixTimestamp>(timestampMs / 1000)).ToString_Time();
        }

        std::string ConfigurationLabel(const std::size_t inputWindowSeconds, const std::size_t matchCount) {
            return std::format("{}s input / {}-match cap", inputWindowSeconds, matchCount);
        }

    }

    // ========== PREDICTION EXPERIMENT ========== //

    PredictionExperiment::PredictionExperiment() {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            slots_[i].input_window_seconds = Core::INPUT_WINDOW_CONFIGURATIONS[i];
        }
    }

    // === OBSERVATION (stream thread) === //

    void PredictionExperiment::ObserveRecord(const LiveRecord& record) {
        for (WindowSlot& slot : slots_) {
            for (std::size_t i = 0; i < PREDICTION_MATCH_COUNTS.size(); ++i) {
                LogReports(
                    slot.analytics[i].ObserveRecord(record.timestamp_ms, record.record.mid_delta, record.observed),
                    slot.input_window_seconds,
                    PREDICTION_MATCH_COUNTS[i]
                );
            }
        }
    }

    void PredictionExperiment::FlushIncomplete() {
        for (WindowSlot& slot : slots_) {
            for (std::size_t i = 0; i < PREDICTION_MATCH_COUNTS.size(); ++i) {
                LogReports(slot.analytics[i].FlushIncomplete(), slot.input_window_seconds, PREDICTION_MATCH_COUNTS[i]);
            }
        }
    }

    // === PREDICTION (prediction thread) === //

    void PredictionExperiment::RunSnapshot(const LiveInputWindow::Snapshot& snapshot) {
        const std::optional<std::size_t> lastSecond = GetSecondsSinceMarketOpen(snapshot.last_timestamp_ms);

        // Pre-market and weekend data has no historical session to compare against.
        if (lastSecond.has_value() == false) {
            return;
        }

        std::array<ForecastRow, Core::INPUT_WINDOW_CONFIGURATIONS.size()> rows{};

        for (std::size_t i = 0; i < slots_.size(); ++i) {
            WindowSlot& slot = slots_[i];

            // Skipped until the window no longer reaches back into pre-market seconds.
            const std::optional<std::size_t> windowStart = GetWindowStartSecond(*lastSecond, slot.input_window_seconds);
            if (windowStart.has_value() == false) {
                continue;
            }

            // Indexes file each window under the segment it starts in, so look it up the same way.
            EnsureIndexLoaded(slot, *windowStart / SECONDS_PER_SEGMENT);
            if (slot.index == nullptr) {
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
    }

    void PredictionExperiment::EnsureIndexLoaded(WindowSlot& slot, const std::size_t segment) {
        if (slot.loaded_segment == segment) {
            return;
        }

        slot.index.reset();
        slot.loaded_segment = segment;

        const std::size_t segmentStartSecond = segment * SECONDS_PER_SEGMENT;

        // Must check before constructing: a missing sub-index would otherwise be created empty and saved on destruction.
        if (Core::HalfHourMultiIndex::FilesExist(segmentStartSecond, slot.input_window_seconds) == false) {
            Logger::Warning(std::format(
                "No complete index files for the {}s input window in segment {}; that configuration is skipped.",
                slot.input_window_seconds,
                segment
            ));
            return;
        }

        try {
            slot.index = std::make_unique<Core::HalfHourMultiIndex>(segmentStartSecond, slot.input_window_seconds);
        }
        catch (const std::exception& error) {
            Logger::Error(
                std::format("Failed to load the {}s input index for segment {}: {}", slot.input_window_seconds, segment, error.what()),
                true
            );
        }
    }

    PredictionExperiment::ForecastRow PredictionExperiment::RunWindow(WindowSlot& slot, const LiveInputWindow::Snapshot& snapshot) {
        const std::size_t inputWindowSeconds = slot.input_window_seconds;
        ForecastRow row{};

        const std::span<const Core::MLRecord> queryWindow = Core::LatestInputWindow(
            std::span<const Core::MLRecord>(snapshot.records),
            inputWindowSeconds
        );

        // The multi-index splits the raw window into |mid_delta| (normalized) and sign(mid_delta) queries.
        const Core::HalfHourMultiIndex::MultiQueryResult results = slot.index->Query(queryWindow, MAX_QUERY_RESULTS);

        const std::vector<HistoricalMatch> magnitudeMatches =
            CollectHistoricalMatches(results.MagnitudeResults, inputWindowSeconds, MatchWeight, "magnitude");
        const std::vector<HistoricalMatch> directionMatches =
            CollectHistoricalMatches(results.DirectionResults, inputWindowSeconds, DirectionMatchWeight, "direction");

        if (magnitudeMatches.empty() || directionMatches.empty()) {
            Logger::Warning(std::format(
                "{}s input: not enough historical matches with a full future ({} magnitude, {} direction).",
                inputWindowSeconds,
                magnitudeMatches.size(),
                directionMatches.size()
            ));
            return row;
        }

        // Matches are sorted nearest first, so each match count uses a prefix of the same lists.
        for (std::size_t i = 0; i < PREDICTION_MATCH_COUNTS.size(); ++i) {
            const std::size_t matchCount = PREDICTION_MATCH_COUNTS[i];

            const ForecastSummary summary = BuildForecast(
                std::span<const HistoricalMatch>(magnitudeMatches).first(std::min(matchCount, magnitudeMatches.size())),
                std::span<const HistoricalMatch>(directionMatches).first(std::min(matchCount, directionMatches.size())),
                snapshot.last_timestamp_ms
            );

            LogForecast(summary, inputWindowSeconds, matchCount);
            LogReports(slot.analytics[i].SubmitForecast(summary.forecast), inputWindowSeconds, matchCount);
            row[i] = summary.forecast;
        }

        return row;
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
        }

        Logger::Info(std::format("{}s input ({}): {} usable match distances [{}]", inputWindowSeconds, label, matches.size(), distances));

        return matches;
    }

    // === LOGGING === //

    void PredictionExperiment::LogReports(
        const std::vector<PredictionEvaluationReport>& reports,
        const std::size_t inputWindowSeconds,
        const std::size_t matchCount
    ) {
        for (const PredictionEvaluationReport& report : reports) {
            Logger::Info(
                std::format("{}\n{}", ConfigurationLabel(inputWindowSeconds, matchCount), PredictionAnalytics::FormatReport(report)),
                true
            );
        }
    }

    void PredictionExperiment::LogForecast(
        const ForecastSummary& summary,
        const std::size_t inputWindowSeconds,
        const std::size_t matchCount
    ) {
        std::string message = std::format(
            "{}: magnitude {} matches ({:.2f} effective), direction {} matches ({:.2f} effective)\n  {:<8}{:>14}{:>14}{:>12}",
            ConfigurationLabel(inputWindowSeconds, matchCount),
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

    void PredictionExperiment::LogComparison(
        const std::int64_t asOfTimestampMs,
        const std::array<ForecastRow, Core::INPUT_WINDOW_CONFIGURATIONS.size()>& rows
    ) {
        std::string comparison = std::format(
            "Paired forecasts @{} (same live snapshot; next {} seconds)",
            FormatTime(asOfTimestampMs),
            FORECAST_HORIZON_SECONDS
        );

        for (std::size_t window = 0; window < rows.size(); ++window) {
            for (std::size_t i = 0; i < PREDICTION_MATCH_COUNTS.size(); ++i) {
                const std::optional<PredictionForecast>& forecast = rows[window][i];

                comparison += "\n" + ConfigurationLabel(Core::INPUT_WINDOW_CONFIGURATIONS[window], PREDICTION_MATCH_COUNTS[i]);

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

        Logger::Info(comparison, true);
    }

    // ========== END PREDICTION EXPERIMENT ========== //

}
