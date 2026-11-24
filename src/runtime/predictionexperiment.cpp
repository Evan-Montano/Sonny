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
            return Common::DateTime::FromTimestampMilliseconds(timestampMs).ToString_Time();
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

    PredictionExperiment::PredictionExperiment(
        std::vector<std::size_t> inputWindows,
        const std::size_t maxLoadedSegments
    )
        : ConfiguredInputWindows(std::move(inputWindows)),
          MaxLoadedSegments(maxLoadedSegments),
          Slots(ConfiguredInputWindows.size()) {
        if (MaxLoadedSegments == 0) {
            throw std::invalid_argument("At least one half-hour segment must be allowed to load.");
        }

        for (std::size_t i = 0; i < Slots.size(); ++i) {
            if (Core::IsSupportedInputWindow(ConfiguredInputWindows[i]) == false) {
                throw std::invalid_argument(std::format("Unsupported input window length: {}", ConfiguredInputWindows[i]));
            }

            Slots[i].InputWindowSeconds = ConfiguredInputWindows[i];
        }
    }

    const std::vector<std::size_t>& PredictionExperiment::InputWindows() const {
        return ConfiguredInputWindows;
    }

    // === OBSERVATION (stream thread) === //

    void PredictionExperiment::ObserveRecord(const LiveRecord& record) {
        for (WindowSlot& slot : Slots) {
            for (std::size_t i = 0; i < PREDICTION_MATCH_COUNTS.size(); ++i) {
                LogReports(
                    slot.Analytics[i].ObserveRecord(record.TimestampMs, record.Record.MidDelta, record.Observed),
                    slot.InputWindowSeconds,
                    PREDICTION_MATCH_COUNTS[i]
                );
            }
        }
    }

    void PredictionExperiment::FlushIncomplete() {
        for (WindowSlot& slot : Slots) {
            for (std::size_t i = 0; i < PREDICTION_MATCH_COUNTS.size(); ++i) {
                LogReports(slot.Analytics[i].FlushIncomplete(), slot.InputWindowSeconds, PREDICTION_MATCH_COUNTS[i]);
            }
        }
    }

    // === PREDICTION (prediction thread) === //

    void PredictionExperiment::Preload(const std::int64_t timestampMs) {
        // Before the open (and on weekends) the next thing needed is the start of a session.
        const std::size_t currentSegment = GetSecondsSinceMarketOpen(timestampMs).value_or(0) / SECONDS_PER_SEGMENT;
        for (WindowSlot& slot : Slots) {
            EnsureIndexLoaded(slot, currentSegment);
        }
    }

    std::vector<PredictionExperiment::ForecastRow> PredictionExperiment::RunSnapshot(const LiveInputWindow::Snapshot& snapshot) {
        const std::optional<std::size_t> lastSecond = GetSecondsSinceMarketOpen(snapshot.LastTimestampMs);

        // Pre-market and weekend data has no historical session to compare against.
        if (lastSecond.has_value() == false) {
            return {};
        }

        std::vector<ForecastRow> rows(Slots.size());

        for (std::size_t i = 0; i < Slots.size(); ++i) {
            WindowSlot& slot = Slots[i];

            // Skipped until the window no longer reaches back into pre-market seconds.
            const std::optional<std::size_t> windowStart = GetWindowStartSecond(*lastSecond, slot.InputWindowSeconds);
            if (windowStart.has_value() == false) {
                continue;
            }

            // The loaded segments follow the time of day; the window's start says which segment it is in,
            // the same way the indexes file each window under the segment it starts in.
            EnsureIndexLoaded(slot, *windowStart / SECONDS_PER_SEGMENT);
            if (slot.Index == nullptr || slot.Index->LoadedSegmentCount() == 0) {
                continue;
            }

            // One configuration failing (bad file, unexpected data) should not stop the others.
            try {
                rows[i] = RunWindow(slot, snapshot);
            }
            catch (const std::exception& error) {
                Logger::Error(std::format("{}s input prediction failed: {}", slot.InputWindowSeconds, error.what()), true);
            }
        }

        LogComparison(snapshot.LastTimestampMs, rows);

        return rows;
    }

    void PredictionExperiment::EnsureIndexLoaded(WindowSlot& slot, const std::size_t currentSegment) {
        // Nothing changes within a segment, which also keeps unavailable segments from being retried every cycle.
        if (slot.CenteredSegment == currentSegment) {
            return;
        }

        slot.CenteredSegment = currentSegment;

        if (slot.Index == nullptr) {
            slot.Index = std::make_unique<Core::SessionMultiIndex>(slot.InputWindowSeconds);
        }

        const std::vector<std::size_t> previouslyLoaded = slot.Index->LoadedSegments();

        // Missing or unreadable segments are logged and skipped by the index itself.
        slot.Index->LoadSegments(Core::SessionMultiIndex::SelectSegments(currentSegment, MaxLoadedSegments));

        const std::vector<std::size_t> loaded = slot.Index->LoadedSegments();

        if (loaded.empty()) {
            Logger::Warning(std::format(
                "No index segments are available for the {}s input window around {}; that configuration is skipped.",
                slot.InputWindowSeconds,
                SegmentStartLabel(std::min(currentSegment, Core::SessionMultiIndex::SEGMENT_COUNT - 1))
            ));
            return;
        }

        if (loaded != previouslyLoaded) {
            Logger::Info(
                std::format(
                    "{}s input: searching {} of {} half-hour segments, {} to {} (limit {}).",
                    slot.InputWindowSeconds,
                    loaded.size(),
                    Core::SessionMultiIndex::SEGMENT_COUNT,
                    SegmentStartLabel(loaded.front()),
                    SegmentStartLabel(loaded.back() + 1),
                    MaxLoadedSegments
                ),
                true
            );
        }
    }

    PredictionExperiment::ForecastRow PredictionExperiment::RunWindow(WindowSlot& slot, const LiveInputWindow::Snapshot& snapshot) {
        const std::size_t inputWindowSeconds = slot.InputWindowSeconds;
        ForecastRow row{};

        const std::span<const Core::MLRecord> queryWindow = Core::LatestInputWindow(
            std::span<const Core::MLRecord>(snapshot.Records),
            inputWindowSeconds
        );

        // Each segment splits the raw window into |MidDelta| (normalized) and sign(MidDelta) queries.
        // The results are already the nearest across every segment, ranked separately per sub-index.
        const Core::HalfHourMultiIndex::MultiQueryResult results = slot.Index->Query(queryWindow, MAX_QUERY_RESULTS);

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
                snapshot.LastTimestampMs
            );

            LogForecast(summary, inputWindowSeconds, matchCount);
            LogReports(slot.Analytics[i].SubmitForecast(summary.Forecast), inputWindowSeconds, matchCount);
            row[i] = summary.Forecast;
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

            if (std::isfinite(result.Distance) == false || result.Distance < 0.0f) {
                continue;
            }

            const std::vector<Core::MLRecord>* day = HistoricalRecords.Find(result.TradingDayId);
            if (day == nullptr) {
                continue;
            }

            // Matches near the session close do not have FORECAST_HORIZON_SECONDS of future left.
            const std::size_t futureStart = Core::HistoricalFutureStart(result.RecordIndex, inputWindowSeconds);
            if (futureStart > day->size() || FORECAST_HORIZON_SECONDS > day->size() - futureStart) {
                continue;
            }

            // The future is the raw signed deltas for both sub-indexes; BuildForecast decides
            // whether to read its magnitude or its direction.
            HistoricalMatch match{.Weight = weightFunction(result.Distance)};
            for (std::size_t second = 0; second < FORECAST_HORIZON_SECONDS; ++second) {
                match.MidDeltas[second] = (*day)[futureStart + second].MidDelta;
            }

            matches.push_back(match);
            distances += std::format("{}{:.4f}", distances.empty() ? "" : ", ", result.Distance);

            // Each segment's index holds the windows that start inside it, so the start names the segment.
            ++segmentOrigins[std::min(
                static_cast<std::size_t>(result.RecordIndex) / SECONDS_PER_SEGMENT,
                Core::SessionMultiIndex::SEGMENT_COUNT - 1
            )];
            tradingDays.insert(result.TradingDayId);
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
            summary.MagnitudeMatches,
            summary.MagnitudeEffectiveMatches,
            summary.DirectionMatches,
            summary.DirectionEffectiveMatches,
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
                std::format("${:+.5f}", summary.Forecast.CumulativeChanges[i]),
                std::format("${:.5f}", summary.ExpectedMagnitudes[i]),
                summary.DirectionAgreement[i]
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

                comparison += "\n" + ConfigurationLabel(ConfiguredInputWindows[window], PREDICTION_MATCH_COUNTS[i]);

                if (forecast.has_value() == false) {
                    comparison += " | unavailable";
                    continue;
                }

                comparison += std::format(" | {} matches", forecast->HistoricalMatches);

                for (std::size_t h = 0; h < PredictionEvaluationHorizons.size(); ++h) {
                    comparison += std::format(" | {:>2}s ${:+.5f}", PredictionEvaluationHorizons[h], forecast->CumulativeChanges[h]);
                }
            }
        }

        Logger::Info(comparison, true);
    }

    // ========== END PREDICTION EXPERIMENT ========== //

}
