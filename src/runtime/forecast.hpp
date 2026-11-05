// forecast.hpp
// Combines the futures of historical matches into one distance-weighted forecast.

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
     * @brief A forecast plus the diagnostics logged alongside it.
     */
    struct ForecastSummary {
        /**
         * @brief Weighted mean cumulative midpoint change at each evaluation horizon.
         */
        PredictionForecast forecast;

        /**
         * @brief Kish effective sample size: how many equally weighted matches the weights amount to.
         */
        double effective_matches{};

        /**
         * @brief Weighted mean of |cumulative move| at each evaluation horizon.
         * Comparing |forecast| to this shows whether the matches agreed on a direction.
         */
        std::array<double, PredictionEvaluationHorizons.size()> mean_absolute_moves{};
    };

    /**
     * @brief Weight for a match given its FAISS distance.
     *
     * IndexFlatL2 reports squared L2 distance, so 1 / d^2 falls off with the fourth power of the
     * true distance. A small epsilon keeps exact matches finite.
     *
     * @param squaredDistance Distance returned by HalfHourIndex::Query.
     * @return Positive weight.
     */
    double MatchWeight(float squaredDistance);

    /**
     * @brief Build a forecast from matches, nearest first.
     *
     * @param matches At least one match.
     * @param asOfTimestampMs Timestamp of the newest live record; the forecast covers the seconds after it.
     * @throws std::invalid_argument If matches is empty or the weights do not sum to a positive value.
     */
    ForecastSummary BuildForecast(std::span<const HistoricalMatch> matches, std::int64_t asOfTimestampMs);

}
