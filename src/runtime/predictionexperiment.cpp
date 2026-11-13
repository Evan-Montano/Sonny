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

        std::string ConfigurationLabel(const std::size_t inputWindowSeconds, const std::size_t matchCount) {
            return std::format("{}s input / {}-match cap", inputWindowSeconds, matchCount);
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

    PredictionExperiment::PredictionExperiment(std::vector<std::size_t> inputWindows)
        : input_windows_(std::move(inputWindows)),
          slots_(input_windows_.size()) {
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

    std::vector<PredictionExperiment::ForecastRow> PredictionExperiment::RunSnapshot(const LiveInputWindow::Snapshot& snapshot) {
        const std::optional<std::size_t> lastSecond = GetSecondsSinceMarketOpen(snapshot.last_timestamp_ms);

        // Pre-market and weekend data has no historical session to compare against.
        if (lastSecond.has_value() == false) {
            return {};
        }

        std::vector<ForecastRow> rows(slots_.size());

        for (std::size_t i = 0; i < slots_.size(); ++i) {
            WindowSlot& slot = slots_[i];

            // Skipped until the window no longer reaches back into pre-market seconds.
            const std::optional<std::size_t> windowStart = GetWindowStartSecond(*lastSecond, slot.input_window_seconds);
            if (windowStart.has_value() == false) {
                continue;
            }

            // Every half-hour segment is searched, whichever one the live window falls in.
            EnsureIndexLoaded(slot);
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

        return rows;
    }

    void PredictionExperiment::EnsureIndexLoaded(WindowSlot& slot) {
        if (slot.load_attempted) {
            return;
        }

        slot.load_attempted = true;

        // Missing or unreadable segments are logged and skipped by the index itself.
        try {
            slot.index = std::make_unique<Core::SessionMultiIndex>(slot.input_window_seconds);
        }
        catch (const std::exception& error) {
            Logger::Error(std::format("Failed to load the {}s input indexes: {}", slot.input_window_seconds, error.what()), true);
            return;
        }

        const std::size_t loadedSegments = slot.index->LoadedSegmentCount();

        if (loadedSegments == 0) {
            Logger::Warning(std::format(
                "No index segments are available for the {}s input window; that configuration is skipped.",
                slot.input_window_seconds
            ));
            slot.index.reset();
            return;
        }

        Logger::Info(
            std::format(
                "{}s input: searching {} of {} half-hour segments.",
                slot.input_window_seconds,
                loadedSegments,
                Core::SessionMultiIndex::SEGMENT_COUNT
            ),
            true
        );
    }

    PredictionExperiment::ForecastRow PredictionExperiment::RunWindow(WindowSlot& slot, const LiveInputWindow::Snapshot& snapshot) {
        const std::size_t inputWindowSeconds = slot.input_window_seconds;
        ForecastRow row{};

        const std::span<const Core::MLRecord> queryWindow = Core::LatestInputWindow(
            std::span<const Core::MLRecord>(snapshot.records),
            inputWindowSeconds
        );

        // Each segment splits the raw window into |mid_delta| (normalized) and sign(mid_delta) queries.
        // The results are already the nearest across every segment, ranked separately per sub-index.
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

    void PredictionExperiment::LogComparison(const std::int64_t asOfTimestampMs, const std::vector<ForecastRow>& rows) const {
        std::string comparison = std::format(
            "Paired forecasts @{} (same live snapshot; next {} seconds)",
            FormatTime(asOfTimestampMs),
            FORECAST_HORIZON_SECONDS
        );

        for (std::size_t window = 0; window < rows.size(); ++window) {
            for (std::size_t i = 0; i < PREDICTION_MATCH_COUNTS.size(); ++i) {
                const std::optional<PredictionForecast>& forecast = rows[window][i];

                comparison += "\n" + ConfigurationLabel(input_windows_[window], PREDICTION_MATCH_COUNTS[i]);

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
