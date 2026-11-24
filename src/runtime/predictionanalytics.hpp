// predictionanalytics.hpp
// Scores forecasts against the live one-second records that follow them.

#pragma once

#include "onesecondaggregator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace Runtime {

    /**
     * @brief Seconds after the forecast at which predicted and actual cumulative moves are compared.
     */
    inline constexpr auto PredictionEvaluationHorizons = std::to_array<std::size_t>({
        1, 2, 3, 4, 5
    });

    /**
     * @brief Largest horizon retained after the input portion of every live snapshot.
     */
    inline constexpr std::size_t PredictionEvaluationWindow = std::ranges::max(PredictionEvaluationHorizons);

    /**
     * @brief A forecast as submitted for scoring.
     */
    struct PredictionForecast {
        // MEMBERS

        /**
         * @brief Timestamp of the newest record the forecast saw. Second 1 of the future is the next second.
         */
        std::int64_t AsOfTimestampMs{};

        /**
         * @brief Number of historical matches the forecast averaged.
         */
        std::size_t HistoricalMatches{};

        /**
         * @brief Predicted cumulative midpoint change at each PredictionEvaluationHorizons entry.
         */
        std::array<double, PredictionEvaluationHorizons.size()> CumulativeChanges{};
    };

    /**
     * @brief Predicted versus actual change at one horizon.
     */
    struct PredictionHorizonEvaluation {
        // MEMBERS

        std::size_t HorizonSeconds{};
        double PredictedChange{};

        /**
         * @brief Set only when every second up to the horizon was actually observed.
         */
        std::optional<double> ActualChange;

        /**
         * @brief Observed seconds up to the horizon.
         */
        std::size_t ObservedSeconds{};
    };

    /**
     * @brief Result of scoring one forecast.
     */
    struct PredictionEvaluationReport {
        // MEMBERS

        PredictionForecast Forecast;

        /**
         * @brief Observed seconds across the whole evaluation window.
         */
        std::size_t ObservedSeconds{};

        std::array<PredictionHorizonEvaluation, PredictionEvaluationHorizons.size()> Horizons{};
    };

    /**
     * @brief Scores a forecast immediately against the held-out future from the same snapshot.
     *
     * No pending forecasts or stream-thread history are retained. Carried-forward seconds make
     * horizons that include them incomplete. Independent evaluations share no mutable state.
     */
    class PredictionAnalytics {
    public:
        // METHODS

        /**
         * @brief Compare a forecast with exactly the following maximum-horizon seconds.
         * @throws std::invalid_argument If the future has the wrong length or timestamps.
         */
        static PredictionEvaluationReport Evaluate(
            const PredictionForecast& forecast,
            std::span<const LiveRecord> future
        );

        /** @brief Format a held-out evaluation as a log-friendly table. */
        static std::string FormatReport(const PredictionEvaluationReport& report);
    };
}
