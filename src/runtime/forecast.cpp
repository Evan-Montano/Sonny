// forecast.cpp
// Implementation of the combined magnitude × direction historical-match forecast.

#include "forecast.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace Runtime {

    namespace {

        /**
         * @brief Running weight totals for one index's matches.
         */
        struct WeightTotals {
            double total{};
            double squared{};

            double EffectiveMatches() const {
                return (total * total) / squared;
            }
        };

        void ValidateMatches(const std::span<const HistoricalMatch> matches, const WeightTotals& weights, const char* label) {
            if (matches.empty()) {
                throw std::invalid_argument(std::string("Cannot build a forecast without ") + label + " matches.");
            }

            if (weights.total > 0.0 == false || std::isfinite(weights.total) == false) {
                throw std::invalid_argument(std::string("The ") + label + " match weights must sum to a finite positive value.");
            }
        }

        int MoveDirection(const double cumulative) {
            if (cumulative > FLAT_MOVE_EPSILON) return 1;
            if (cumulative < -FLAT_MOVE_EPSILON) return -1;
            return 0;
        }

    }

    double MatchWeight(const float squaredDistance) {
        constexpr double DISTANCE_EPSILON = 1e-6;
        constexpr double DISTANCE_POWER = 2.0;

        return 1.0 / std::pow(static_cast<double>(squaredDistance) + DISTANCE_EPSILON, DISTANCE_POWER);
    }

    double DirectionMatchWeight(const float squaredDistance) {
        return 1.0 / (1.0 + static_cast<double>(std::max(squaredDistance, 0.0f)));
    }

    ForecastSummary BuildForecast(
        const std::span<const HistoricalMatch> magnitudeMatches,
        const std::span<const HistoricalMatch> directionMatches,
        const std::int64_t asOfTimestampMs
    ) {
        // === MAGNITUDE: weighted mean of |cumulative move| per future second === //

        std::array<double, FORECAST_HORIZON_SECONDS> weightedAbsoluteCumulative{};
        WeightTotals magnitudeWeights{};

        for (const HistoricalMatch& match : magnitudeMatches) {
            magnitudeWeights.total += match.weight;
            magnitudeWeights.squared += match.weight * match.weight;

            double cumulative = 0.0;
            for (std::size_t second = 0; second < FORECAST_HORIZON_SECONDS; ++second) {
                cumulative += match.mid_deltas[second];
                weightedAbsoluteCumulative[second] += match.weight * std::abs(cumulative);
            }
        }

        ValidateMatches(magnitudeMatches, magnitudeWeights, "magnitude");

        // === DIRECTION: weighted vote on the sign of the cumulative move per future second === //

        std::array<double, FORECAST_HORIZON_SECONDS> weightedDirectionVotes{};
        WeightTotals directionWeights{};

        for (const HistoricalMatch& match : directionMatches) {
            directionWeights.total += match.weight;
            directionWeights.squared += match.weight * match.weight;

            double cumulative = 0.0;
            for (std::size_t second = 0; second < FORECAST_HORIZON_SECONDS; ++second) {
                cumulative += match.mid_deltas[second];
                weightedDirectionVotes[second] += match.weight * MoveDirection(cumulative);
            }
        }

        ValidateMatches(directionMatches, directionWeights, "direction");

        // === COMBINE === //

        ForecastSummary summary{
            .forecast = PredictionForecast{
                .as_of_timestamp_ms = asOfTimestampMs,
                .historical_matches = std::min(magnitudeMatches.size(), directionMatches.size())
            },
            .magnitude_matches = magnitudeMatches.size(),
            .direction_matches = directionMatches.size(),
            .magnitude_effective_matches = magnitudeWeights.EffectiveMatches(),
            .direction_effective_matches = directionWeights.EffectiveMatches()
        };

        // Horizons are 1-based seconds; the arrays above are 0-based.
        for (std::size_t i = 0; i < PredictionEvaluationHorizons.size(); ++i) {
            const std::size_t second = PredictionEvaluationHorizons[i] - 1;

            const double magnitude = weightedAbsoluteCumulative[second] / magnitudeWeights.total;
            const double agreement = weightedDirectionVotes[second] / directionWeights.total;

            summary.expected_magnitudes[i] = magnitude;
            summary.direction_agreement[i] = agreement;

            // Full magnitude in the winning direction; the agreement is kept as a confidence diagnostic
            // instead of scaling the magnitude, which would reintroduce the shrink-toward-zero problem.
            const double direction = agreement > 0.0 ? 1.0 : (agreement < 0.0 ? -1.0 : 0.0);
            summary.forecast.cumulative_changes[i] = direction * magnitude;
        }

        return summary;
    }

}
