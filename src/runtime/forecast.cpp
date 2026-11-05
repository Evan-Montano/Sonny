// forecast.cpp
// Implementation of the weighted historical-match forecast.

#include "forecast.hpp"

#include <cmath>
#include <stdexcept>

namespace Runtime {

    double MatchWeight(const float squaredDistance) {
        constexpr double DISTANCE_EPSILON = 1e-6;
        constexpr double DISTANCE_POWER = 2.0;

        return 1.0 / std::pow(static_cast<double>(squaredDistance) + DISTANCE_EPSILON, DISTANCE_POWER);
    }

    ForecastSummary BuildForecast(const std::span<const HistoricalMatch> matches, const std::int64_t asOfTimestampMs) {
        if (matches.empty()) {
            throw std::invalid_argument("Cannot build a forecast without historical matches.");
        }

        // Weighted sums of each match's cumulative path, and of its absolute value, per future second.
        std::array<double, FORECAST_HORIZON_SECONDS> weightedCumulative{};
        std::array<double, FORECAST_HORIZON_SECONDS> weightedAbsoluteCumulative{};
        double totalWeight = 0.0;
        double totalSquaredWeight = 0.0;

        for (const HistoricalMatch& match : matches) {
            totalWeight += match.weight;
            totalSquaredWeight += match.weight * match.weight;

            double cumulative = 0.0;
            for (std::size_t second = 0; second < FORECAST_HORIZON_SECONDS; ++second) {
                cumulative += match.mid_deltas[second];
                weightedCumulative[second] += match.weight * cumulative;
                weightedAbsoluteCumulative[second] += match.weight * std::abs(cumulative);
            }
        }

        if (totalWeight > 0.0 == false || std::isfinite(totalWeight) == false) {
            throw std::invalid_argument("Historical match weights must sum to a finite positive value.");
        }

        ForecastSummary summary{
            .forecast = PredictionForecast{
                .as_of_timestamp_ms = asOfTimestampMs,
                .historical_matches = matches.size()
            },
            .effective_matches = (totalWeight * totalWeight) / totalSquaredWeight
        };

        // Horizons are 1-based seconds; the arrays above are 0-based.
        for (std::size_t i = 0; i < PredictionEvaluationHorizons.size(); ++i) {
            const std::size_t second = PredictionEvaluationHorizons[i] - 1;
            summary.forecast.cumulative_changes[i] = weightedCumulative[second] / totalWeight;
            summary.mean_absolute_moves[i] = weightedAbsoluteCumulative[second] / totalWeight;
        }

        return summary;
    }

}
