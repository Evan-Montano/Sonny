// predictionanalytics.cpp
// Implementation of forecast scoring against live one-second records.

#include "predictionanalytics.hpp"

#include "common/datetime.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace Runtime {
    namespace {
        constexpr std::int64_t MillisecondsPerSecond = 1000;
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

    PredictionEvaluationReport PredictionAnalytics::Evaluate(
        const PredictionForecast& forecast,
        const std::span<const LiveRecord> future
    ) {
        if (future.size() != PredictionEvaluationWindow) {
            throw std::invalid_argument("Evaluation requires the full held-out future.");
        }

        const std::int64_t anchorSecond = forecast.AsOfTimestampMs / MillisecondsPerSecond;
        for (std::size_t i = 0; i < future.size(); ++i) {
            if (future[i].TimestampMs / MillisecondsPerSecond != anchorSecond + static_cast<std::int64_t>(i) + 1) {
                throw std::invalid_argument("Evaluation future must immediately follow the input, one record per second.");
            }
        }

        PredictionEvaluationReport report{.Forecast = forecast};
        report.ObservedSeconds = std::count_if(future.begin(), future.end(), [](const LiveRecord& record) {
            return record.Observed;
        });

        for (std::size_t i = 0; i < PredictionEvaluationHorizons.size(); ++i) {
            PredictionHorizonEvaluation& evaluation = report.Horizons[i];
            evaluation.HorizonSeconds = PredictionEvaluationHorizons[i];
            evaluation.PredictedChange = forecast.CumulativeChanges[i];

            double actualChange = 0.0;
            for (const LiveRecord& record : future.first(evaluation.HorizonSeconds)) {
                if (record.Observed) {
                    actualChange += record.Record.MidDelta;
                    ++evaluation.ObservedSeconds;
                }
            }

            if (evaluation.ObservedSeconds == evaluation.HorizonSeconds) {
                // Avoid reporting tiny floating-point residue as a directional move.
                if (std::abs(actualChange) < DirectionEpsilon) actualChange = 0.0;
                evaluation.ActualChange = actualChange;
            }
        }

        return report;
    }

    std::string PredictionAnalytics::FormatReport(const PredictionEvaluationReport& report) {
        const std::string timestamp = Common::DateTime::FromTimestampMilliseconds(report.Forecast.AsOfTimestampMs).ToString_Time();

        std::string message = std::format(
            "Held-out evaluation @{} | {} matches | observed {}/{}\n"
            "  {:<8}{:>14}{:>14}{:>14}{:>14}{:>16}{:>10}",
            timestamp,
            report.Forecast.HistoricalMatches,
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
