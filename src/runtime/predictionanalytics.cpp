#include "predictionanalytics.hpp"

#include "common/datetime.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace Runtime {
    namespace {
        constexpr std::int64_t MillisecondsPerSecond = 1000;
        constexpr std::int64_t HistoryWindowSeconds = 180;
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

    std::int64_t PredictionAnalytics::ToSecond(const std::int64_t timestamp_ms) {
        return timestamp_ms / MillisecondsPerSecond;
    }

    PredictionEvaluationReport PredictionAnalytics::BuildReport(
        const PendingForecast& pending,
        const bool window_elapsed
    ) {
        PredictionEvaluationReport report{
            .forecast = pending.forecast,
            .window_elapsed = window_elapsed
        };

        for (std::size_t i = 0; i < pending.future.size(); ++i) {
            if (pending.future[i].has_value() && pending.future[i]->observed) {
                ++report.observed_seconds;
            }
        }

        for (std::size_t horizon_index = 0; horizon_index < PredictionEvaluationHorizons.size(); ++horizon_index) {
            const std::size_t horizon = PredictionEvaluationHorizons[horizon_index];
            PredictionHorizonEvaluation& evaluation = report.horizons[horizon_index];
            evaluation.horizon_seconds = horizon;
            evaluation.predicted_change = pending.forecast.cumulative_changes[horizon_index];

            double actual_change = 0.0;
            bool complete = true;
            for (std::size_t i = 0; i < horizon; ++i) {
                const std::optional<Observation>& observation = pending.future[i];
                if (observation.has_value() == false || observation->observed == false) {
                    complete = false;
                    continue;
                }

                actual_change += observation->mid_delta;
                ++evaluation.observed_seconds;
            }

            if (complete) {
                // Avoid reporting tiny floating-point residue as a directional move.
                if (std::abs(actual_change) < DirectionEpsilon) actual_change = 0.0;
                evaluation.actual_change = actual_change;
            }
        }

        return report;
    }

    std::vector<PredictionEvaluationReport> PredictionAnalytics::SubmitForecast(PredictionForecast forecast) {
        std::lock_guard lock(mutex_);

        PendingForecast pending{.forecast = std::move(forecast)};
        const std::int64_t anchor_second = ToSecond(pending.forecast.as_of_timestamp_ms);
        const std::int64_t end_second = anchor_second + static_cast<std::int64_t>(PredictionEvaluationWindow);

        for (const Observation& observation : history_) {
            const std::int64_t offset = observation.second - anchor_second;
            if (offset < 1 || offset > static_cast<std::int64_t>(PredictionEvaluationWindow)) continue;
            std::optional<Observation>& slot = pending.future[static_cast<std::size_t>(offset - 1)];
            if (slot.has_value() == false || (observation.observed && !slot->observed)) {
                slot = observation;
            }
        }

        if (latest_second_.has_value() && *latest_second_ >= end_second) {
            return {BuildReport(pending, true)};
        }

        pending_.push_back(std::move(pending));
        return {};
    }

    std::vector<PredictionEvaluationReport> PredictionAnalytics::ObserveRecord(
        const std::int64_t timestamp_ms,
        const double mid_delta,
        const bool observed
    ) {
        std::lock_guard lock(mutex_);

        const std::int64_t second = ToSecond(timestamp_ms);
        Observation observation{second, mid_delta, observed};
        latest_second_ = std::max(latest_second_.value_or(second), second);

        if (history_.empty() == false && history_.back().second == second) {
            if (observed || history_.back().observed == false) history_.back() = observation;
        } else {
            history_.push_back(observation);
        }

        while (history_.empty() == false && latest_second_.value() - history_.front().second > HistoryWindowSeconds) {
            history_.pop_front();
        }

        std::vector<PredictionEvaluationReport> completed;
        auto pending = pending_.begin();
        while (pending != pending_.end()) {
            const std::int64_t anchor_second = ToSecond(pending->forecast.as_of_timestamp_ms);
            const std::int64_t offset = second - anchor_second;

            if (offset >= 1 && offset <= static_cast<std::int64_t>(PredictionEvaluationWindow)) {
                std::optional<Observation>& slot = pending->future[static_cast<std::size_t>(offset - 1)];
                if (slot.has_value() == false || (observed && !slot->observed)) slot = observation;
            }

            if (latest_second_.value() >= anchor_second + static_cast<std::int64_t>(PredictionEvaluationWindow)) {
                completed.push_back(BuildReport(*pending, true));
                pending = pending_.erase(pending);
            } else {
                ++pending;
            }
        }

        return completed;
    }

    std::vector<PredictionEvaluationReport> PredictionAnalytics::FlushIncomplete() {
        std::lock_guard lock(mutex_);

        std::vector<PredictionEvaluationReport> incomplete;
        incomplete.reserve(pending_.size());
        for (const PendingForecast& pending : pending_) {
            incomplete.push_back(BuildReport(pending, false));
        }
        pending_.clear();
        return incomplete;
    }

    std::string PredictionAnalytics::FormatReport(const PredictionEvaluationReport& report) {
        const std::string timestamp = Common::DateTime(
            static_cast<Common::UnixTimestamp>(ToSecond(report.forecast.as_of_timestamp_ms))
        ).ToString_Time();

        std::string message = std::format(
            "Prediction evaluation @{} | {} matches | {} window | observed {}/{}\n"
            "  Horizon   Forecast         Actual           Error       Abs error   Zero baseline   Direction",
            timestamp,
            report.forecast.historical_matches,
            report.window_elapsed ? "complete" : "incomplete",
            report.observed_seconds,
            PredictionEvaluationWindow
        );

        for (const PredictionHorizonEvaluation& horizon : report.horizons) {
            message += std::format("\n  {:>2}s", horizon.horizon_seconds);
            if (horizon.actual_change.has_value()) {
                const double error = horizon.predicted_change - *horizon.actual_change;
                message += std::format(
                    "   ${:>+11.6f}   ${:>+11.6f}   ${:>+11.6f}   ${:>10.6f}   ${:>10.6f}   {}",
                    horizon.predicted_change,
                    *horizon.actual_change,
                    error,
                    std::abs(error),
                    std::abs(*horizon.actual_change),
                    DirectionResult(horizon.predicted_change, *horizon.actual_change)
                );
            } else {
                message += std::format(
                    "   ${:>+11.6f}   {:>12}   {:>12}   {:>10}   {:>13}   N/A ({}/{})",
                    horizon.predicted_change,
                    "INCOMPLETE",
                    "N/A",
                    "N/A",
                    "N/A",
                    horizon.observed_seconds,
                    horizon.horizon_seconds
                );
            }
        }

        return message;
    }
}
