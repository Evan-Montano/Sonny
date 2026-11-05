// predictionanalytics.hpp
// Scores forecasts against the live one-second records that follow them.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Runtime {

    /**
     * @brief Seconds after the forecast at which predicted and actual cumulative moves are compared.
     */
    inline constexpr std::array<std::size_t, 11> PredictionEvaluationHorizons{
        1, 2, 3, 4, 5, 8, 10, 15, 20, 25, 30
    };

    /**
     * @brief Number of seconds a forecast stays open for evaluation.
     */
    inline constexpr std::size_t PredictionEvaluationWindow = 30;

    static_assert(
        PredictionEvaluationHorizons.back() <= PredictionEvaluationWindow,
        "Every evaluation horizon must fit inside the evaluation window."
    );

    /**
     * @brief A forecast as submitted for scoring.
     */
    struct PredictionForecast {
        /**
         * @brief Timestamp of the newest record the forecast saw. Second 1 of the future is the next second.
         */
        std::int64_t as_of_timestamp_ms{};

        /**
         * @brief Number of historical matches the forecast averaged.
         */
        std::size_t historical_matches{};

        /**
         * @brief Predicted cumulative midpoint change at each PredictionEvaluationHorizons entry.
         */
        std::array<double, PredictionEvaluationHorizons.size()> cumulative_changes{};
    };

    /**
     * @brief Predicted versus actual change at one horizon.
     */
    struct PredictionHorizonEvaluation {
        std::size_t horizon_seconds{};
        double predicted_change{};

        /**
         * @brief Set only when every second up to the horizon was actually observed.
         */
        std::optional<double> actual_change;

        /**
         * @brief Observed seconds up to the horizon.
         */
        std::size_t observed_seconds{};
    };

    /**
     * @brief Result of scoring one forecast.
     */
    struct PredictionEvaluationReport {
        PredictionForecast forecast;

        /**
         * @brief False when the runtime stopped before the evaluation window finished.
         */
        bool window_elapsed{};

        /**
         * @brief Observed seconds across the whole evaluation window.
         */
        std::size_t observed_seconds{};

        std::array<PredictionHorizonEvaluation, PredictionEvaluationHorizons.size()> horizons{};
    };

    /**
     * @brief Scores forecasts against subsequent observed quote deltas.
     *
     * This component is independent of the prediction algorithm: callers submit
     * cumulative forecasts and the one-second observations used as outcomes.
     * Seconds marked unobserved (carried forward) make the horizons they fall in incomplete.
     * All public methods are thread-safe.
     */
    class PredictionAnalytics {
    public:
        // METHODS

        /**
         * @brief Start tracking a forecast. Observations already received for its future are applied immediately.
         *
         * @param forecast Forecast to score.
         * @return A report if its window has already elapsed; otherwise empty.
         */
        std::vector<PredictionEvaluationReport> SubmitForecast(PredictionForecast forecast);

        /**
         * @brief Record one second's outcome and close any forecasts whose window it completes.
         *
         * @param timestamp_ms Timestamp inside the observed second.
         * @param mid_delta Midpoint change during that second.
         * @param observed False if the second was carried forward rather than seen.
         * @return Reports for forecasts that just finished.
         */
        std::vector<PredictionEvaluationReport> ObserveRecord(
            std::int64_t timestamp_ms,
            double mid_delta,
            bool observed = true
        );

        /**
         * @brief Close every open forecast, e.g. when the runtime shuts down.
         *
         * @return Reports marked as incomplete.
         */
        std::vector<PredictionEvaluationReport> FlushIncomplete();

        /**
         * @brief Format a report as a log-friendly table.
         */
        static std::string FormatReport(const PredictionEvaluationReport& report);

    private:
        /**
         * @brief One second's outcome.
         */
        struct Observation {
            std::int64_t second{};
            double mid_delta{};
            bool observed{};
        };

        /**
         * @brief A forecast waiting for its evaluation window to elapse.
         */
        struct PendingForecast {
            PredictionForecast forecast;

            /**
             * @brief Slot i holds the outcome of second i + 1 after the forecast.
             */
            std::array<std::optional<Observation>, PredictionEvaluationWindow> future{};
        };

        // METHODS

        static std::int64_t ToSecond(std::int64_t timestamp_ms);
        static PredictionEvaluationReport BuildReport(const PendingForecast& pending, bool window_elapsed);

        // MEMBERS

        /**
         * @brief Guards every member below.
         */
        std::mutex mutex_;

        /**
         * @brief Recent observations, kept so forecasts submitted late can still be filled in.
         */
        std::deque<Observation> history_;

        /**
         * @brief Forecasts still inside their evaluation window.
         */
        std::vector<PendingForecast> pending_;

        /**
         * @brief Newest second observed so far.
         */
        std::optional<std::int64_t> latest_second_;
    };
}
