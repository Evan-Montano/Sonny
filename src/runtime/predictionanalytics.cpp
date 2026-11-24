// predictionanalytics.cpp
// Implementation of forecast scoring against live one-second records.

#include "predictionanalytics.hpp"

#include "common/datetime.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace Runtime {
    namespace {
        constexpr std::int64_t MillisecondsPerSecond = 1000;
        // Long enough that a slow prediction cycle can still look up every second of its future.
        constexpr std::int64_t HistoryWindowSeconds = 180;

        // Moves smaller than this count as flat when comparing predicted and actual direction.
        constexpr double DirectionEpsilon = 0.0005;

        int Direction(double value) {
            if (value > DirectionEpsilon) return 1;
            if (value < -DirectionEpsilon) return -1;
            return 0;
        }

        const char* DirectionResult(double predicted, double actual) {
            return Direction(predicted) == Direction(actual) ? "HIT" : "MISS";
        }
    }

    // ========== PREDICTION ANALYTICS ========== //

    std::int64_t PredictionAnalytics::ToSecond(const std::int64_t timestamp_ms) {
        return timestamp_ms / MillisecondsPerSecond;
    }

    PredictionEvaluationReport PredictionAnalytics::BuildReport(
        const PendingForecast& pending,
        const bool window_elapsed
    ) {
        PredictionEvaluationReport report{
            .Forecast = pending.Forecast,
            .WindowElapsed = window_elapsed
        };

        for (std::size_t i = 0; i < pending.Future.size(); ++i) {
            if (pending.Future[i].has_value() && pending.Future[i]->Observed) {
                ++report.ObservedSeconds;
            }
        }

        for (std::size_t horizon_index = 0; horizon_index < PredictionEvaluationHorizons.size(); ++horizon_index) {
            const std::size_t horizon = PredictionEvaluationHorizons[horizon_index];
            PredictionHorizonEvaluation& evaluation = report.Horizons[horizon_index];
            evaluation.HorizonSeconds = horizon;
            evaluation.PredictedChange = pending.Forecast.CumulativeChanges[horizon_index];

            double actual_change = 0.0;
            bool complete = true;
            for (std::size_t i = 0; i < horizon; ++i) {
                const std::optional<Observation>& observation = pending.Future[i];
                if (observation.has_value() == false || observation->Observed == false) {
                    complete = false;
                    continue;
                }

                actual_change += observation->MidDelta;
                ++evaluation.ObservedSeconds;
            }

            if (complete) {
                // Avoid reporting tiny floating-point residue as a directional move.
                if (std::abs(actual_change) < DirectionEpsilon) actual_change = 0.0;
                evaluation.ActualChange = actual_change;
            }
        }

        return report;
    }

    std::vector<PredictionEvaluationReport> PredictionAnalytics::SubmitForecast(PredictionForecast forecast) {
        std::lock_guard lock(Mutex);

        PendingForecast pending{.Forecast = std::move(forecast)};
        const std::int64_t anchor_second = ToSecond(pending.Forecast.AsOfTimestampMs);
        const std::int64_t end_second = anchor_second + static_cast<std::int64_t>(PredictionEvaluationWindow);

        // The prediction cycle takes time, so some of this forecast's future may already have arrived.
        for (const Observation& observation : History) {
            const std::int64_t offset = observation.Second - anchor_second;
            if (offset < 1 || offset > static_cast<std::int64_t>(PredictionEvaluationWindow)) continue;
            std::optional<Observation>& slot = pending.Future[static_cast<std::size_t>(offset - 1)];
            if (slot.has_value() == false || (observation.Observed && !slot->Observed)) {
                slot = observation;
            }
        }

        if (LatestSecond.has_value() && *LatestSecond >= end_second) {
            return {BuildReport(pending, true)};
        }

        Pending.push_back(std::move(pending));
        return {};
    }

    std::vector<PredictionEvaluationReport> PredictionAnalytics::ObserveRecord(
        const std::int64_t timestamp_ms,
        const double mid_delta,
        const bool observed
    ) {
        std::lock_guard lock(Mutex);

        const std::int64_t second = ToSecond(timestamp_ms);
        Observation observation{second, mid_delta, observed};
        LatestSecond = std::max(LatestSecond.value_or(second), second);

        if (History.empty() == false && History.back().Second == second) {
            if (observed || History.back().Observed == false) History.back() = observation;
        } else {
            History.push_back(observation);
        }

        while (History.empty() == false && LatestSecond.value() - History.front().Second > HistoryWindowSeconds) {
            History.pop_front();
        }

        std::vector<PredictionEvaluationReport> completed;
        auto pending = Pending.begin();
        while (pending != Pending.end()) {
            const std::int64_t anchor_second = ToSecond(pending->Forecast.AsOfTimestampMs);
            const std::int64_t offset = second - anchor_second;

            if (offset >= 1 && offset <= static_cast<std::int64_t>(PredictionEvaluationWindow)) {
                std::optional<Observation>& slot = pending->Future[static_cast<std::size_t>(offset - 1)];
                if (slot.has_value() == false || (observed && !slot->Observed)) slot = observation;
            }

            if (LatestSecond.value() >= anchor_second + static_cast<std::int64_t>(PredictionEvaluationWindow)) {
                completed.push_back(BuildReport(*pending, true));
                pending = Pending.erase(pending);
            } else {
                ++pending;
            }
        }

        return completed;
    }

    std::vector<PredictionEvaluationReport> PredictionAnalytics::FlushIncomplete() {
        std::lock_guard lock(Mutex);

        std::vector<PredictionEvaluationReport> incomplete;
        incomplete.reserve(Pending.size());
        for (const PendingForecast& pending : Pending) {
            incomplete.push_back(BuildReport(pending, false));
        }
        Pending.clear();
        return incomplete;
    }

    std::string PredictionAnalytics::FormatReport(const PredictionEvaluationReport& report) {
        const std::string timestamp = Common::DateTime::FromTimestampMilliseconds(report.Forecast.AsOfTimestampMs).ToString_Time();

        std::string message = std::format(
            "Prediction evaluation @{} | {} matches | {} window | observed {}/{}\n"
            "  {:<8}{:>14}{:>14}{:>14}{:>14}{:>16}{:>10}",
            timestamp,
            report.Forecast.HistoricalMatches,
            report.WindowElapsed ? "complete" : "incomplete",
            report.ObservedSeconds,
            PredictionEvaluationWindow,
            "Horizon",
            "Forecast",
            "Actual",
            "Error",
            "Abs error",
            "Zero baseline",
            "Direction"
        );

        for (const PredictionHorizonEvaluation& horizon : report.Horizons) {
            const std::string label = std::format("{}s", horizon.HorizonSeconds);
            if (horizon.ActualChange.has_value()) {
                const double error = horizon.PredictedChange - *horizon.ActualChange;
                message += std::format(
                    "\n  {:<8}{:>14}{:>14}{:>14}{:>14}{:>16}{:>10}",
                    label,
                    std::format("${:+.6f}", horizon.PredictedChange),
                    std::format("${:+.6f}", *horizon.ActualChange),
                    std::format("${:+.6f}", error),
                    std::format("${:.6f}", std::abs(error)),
                    std::format("${:.6f}", std::abs(*horizon.ActualChange)),
                    DirectionResult(horizon.PredictedChange, *horizon.ActualChange)
                );
            } else {
                message += std::format(
                    "\n  {:<8}{:>14}{:>14}{:>14}{:>14}{:>16}{:>10}",
                    label,
                    std::format("${:+.6f}", horizon.PredictedChange),
                    "INCOMPLETE",
                    "N/A",
                    "N/A",
                    "N/A",
                    std::format("N/A ({}/{})", horizon.ObservedSeconds, horizon.HorizonSeconds)
                );
            }
        }

        return message;
    }

    // ========== END PREDICTION ANALYTICS ========== //
}
