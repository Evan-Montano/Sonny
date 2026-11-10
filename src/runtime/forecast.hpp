// forecast.hpp
// Combines the futures of magnitude and direction historical matches into one forecast.

#pragma once

#include "predictionanalytics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Runtime {

    /**
     * @brief Number of future seconds read after each historical match.
     */
    inline constexpr std::size_t FORECAST_HORIZON_SECONDS = PredictionEvaluationWindow;

    static_assert(
        PredictionEvaluationHorizons.back() == FORECAST_HORIZON_SECONDS,
        "The longest evaluated horizon must equal the forecast horizon."
    );

    /**
     * @brief What happened after one historical window that resembled the live window.
     */
    struct HistoricalMatch {
        /**
         * @brief Influence on the forecast; closer matches weigh more (see MatchWeight).
         */
        double weight{};

        /**
         * @brief One-second midpoint changes immediately after the matched window.
         */
        std::array<double, FORECAST_HORIZON_SECONDS> mid_deltas{};
    };

    /**
     * @brief A combined magnitude × direction forecast plus the diagnostics logged alongside it.
     */
    struct ForecastSummary {
        /**
         * @brief Combined forecast: sign(direction_agreement) × expected_magnitudes at each evaluation horizon.
         * historical_matches is the smaller of the two match counts below.
         */
        PredictionForecast forecast;

        /**
         * @brief Matches from the magnitude and direction indexes that went into this forecast.
         */
        std::size_t magnitude_matches{};
        std::size_t direction_matches{};

        /**
         * @brief Kish effective sample size of each index's weights: how many equally weighted matches they amount to.
         */
        double magnitude_effective_matches{};
        double direction_effective_matches{};

        /**
         * @brief Weighted mean of |cumulative move| across the magnitude matches at each evaluation horizon.
         * Absolute values cannot cancel out, so this is the expected size of the move regardless of direction.
         */
        std::array<double, PredictionEvaluationHorizons.size()> expected_magnitudes{};

        /**
         * @brief Weighted net vote of the direction matches at each evaluation horizon, in [-1, 1].
         * +1 means every match moved up by that horizon, -1 every match moved down, 0 an even split (or all flat).
         */
        std::array<double, PredictionEvaluationHorizons.size()> direction_agreement{};
    };

    /**
     * @brief Weight for a magnitude match given its FAISS distance.
     *
     * IndexFlatL2 reports squared L2 distance, so 1 / d^2 falls off with the fourth power of the
     * true distance. A small epsilon keeps exact matches finite.
     *
     * @param squaredDistance Distance returned by HalfHourIndex::Query.
     * @return Positive weight.
     */
    double MatchWeight(float squaredDistance);

    /**
     * @brief Weight for a direction match given its FAISS distance.
     *
     * Direction vectors hold only -1, 0, and 1, so squared distances are small integers and exact
     * matches (distance 0) are common. MatchWeight would give those 1e12 times the weight of a
     * near miss; 1 / (1 + d) keeps the ranking but lets near misses still vote.
     *
     * @param squaredDistance Distance returned by HalfHourIndex::Query.
     * @return Positive weight, at most 1.
     */
    double DirectionMatchWeight(float squaredDistance);

    /**
     * @brief Moves smaller than this (in dollars) count as flat when a direction match votes.
     * Matches PredictionAnalytics' threshold so a "flat" vote means the same thing in both places.
     */
    inline constexpr double FLAT_MOVE_EPSILON = 0.0005;

    /**
     * @brief Build a forecast by combining the two indexes' matches.
     *
     * The magnitude matches decide how far the price moves by each horizon (weighted mean of the
     * absolute cumulative move); the direction matches decide which way (sign of their weighted
     * net vote). When the vote is exactly split the forecast for that horizon is 0.
     *
     * @param magnitudeMatches Matches from the magnitude index, nearest first.
     * @param directionMatches Matches from the direction index, nearest first.
     * @param asOfTimestampMs Timestamp of the newest live record; the forecast covers the seconds after it.
     * @throws std::invalid_argument If either list is empty or its weights do not sum to a positive value.
     */
    ForecastSummary BuildForecast(
        std::span<const HistoricalMatch> magnitudeMatches,
        std::span<const HistoricalMatch> directionMatches,
        std::int64_t asOfTimestampMs
    );

}
