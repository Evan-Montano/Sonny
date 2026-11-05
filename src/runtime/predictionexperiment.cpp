// predictionexperiment.cpp
// Implementation of the side-by-side forecast experiment.

#include "predictionexperiment.hpp"
#include "common/datetime.hpp"
#include "common/logger.hpp"
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

        if (Core::HalfHourIndex::FilesExist(segmentStartSecond, slot.input_window_seconds) == false) {
            Logger::Warning(std::format(
                "No complete index files for the {}s input window in segment {}; that configuration is skipped.",
                slot.input_window_seconds,
                segment
            ));
            return;
        }

        try {
            slot.index = std::make_unique<Core::HalfHourIndex>(segmentStartSecond, slot.input_window_seconds);
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

        const std::vector<float> queryVector = slot.index->NormalizeQuery(queryWindow);
        const std::vector<Core::HalfHourIndex::QueryResult> results = slot.index->Query(queryVector, MAX_QUERY_RESULTS);
        const std::vector<HistoricalMatch> matches = CollectHistoricalMatches(results, inputWindowSeconds);

        if (matches.empty()) {
            Logger::Warning(std::format("{}s input: no historical matches had a full future.", inputWindowSeconds));
            return row;
        }

        // Matches are sorted nearest first, so each match count uses a prefix of the same list.
        for (std::size_t i = 0; i < PREDICTION_MATCH_COUNTS.size(); ++i) {
            const std::size_t matchCount = PREDICTION_MATCH_COUNTS[i];
            const std::size_t usedMatches = std::min(matchCount, matches.size());

            const ForecastSummary summary = BuildForecast(
                std::span<const HistoricalMatch>(matches).first(usedMatches),
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
        const std::size_t inputWindowSeconds
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

            HistoricalMatch match{.weight = MatchWeight(result.distance)};
            for (std::size_t second = 0; second < FORECAST_HORIZON_SECONDS; ++second) {
                match.mid_deltas[second] = (*day)[futureStart + second].mid_delta;
            }

            matches.push_back(match);
            distances += std::format("{}{:.4f}", distances.empty() ? "" : ", ", result.distance);
        }

        Logger::Info(std::format("{}s input: {} usable match distances [{}]", inputWindowSeconds, matches.size(), distances));

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
            "{}: {} matches, {:.2f} effective\n  {:<8}{:>14}{:>14}{:>15}",
            ConfigurationLabel(inputWindowSeconds, matchCount),
            summary.forecast.historical_matches,
            summary.effective_matches,
            "Horizon",
            "Forecast",
            "Mean |move|",
            "Concentration"
        );

        for (std::size_t i = 0; i < PredictionEvaluationHorizons.size(); ++i) {
            const double forecast = summary.forecast.cumulative_changes[i];
            const double meanAbsoluteMove = summary.mean_absolute_moves[i];

            // Near 1 when the matches agree on a direction; near 0 when their moves cancel out.
            const double concentration = meanAbsoluteMove > 0.0 ? std::abs(forecast) / meanAbsoluteMove : 0.0;

            message += std::format(
                "\n  {:<8}{:>14}{:>14}{:>15.3f}",
                std::format("{}s", PredictionEvaluationHorizons[i]),
                std::format("${:+.5f}", forecast),
                std::format("${:.5f}", meanAbsoluteMove),
                concentration
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
