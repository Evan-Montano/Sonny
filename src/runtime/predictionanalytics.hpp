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

    inline constexpr std::array<std::size_t, 4> PredictionEvaluationHorizons{1, 5, 10, 30};
    inline constexpr std::size_t PredictionEvaluationWindow = 30;

    struct PredictionForecast {
        std::int64_t as_of_timestamp_ms{};
        std::size_t historical_matches{};
        std::array<double, PredictionEvaluationHorizons.size()> cumulative_changes{};
    };

    struct PredictionHorizonEvaluation {
        std::size_t horizon_seconds{};
        double predicted_change{};
        std::optional<double> actual_change;
        std::size_t observed_seconds{};
    };

    struct PredictionEvaluationReport {
        PredictionForecast forecast;
        bool window_elapsed{};
        std::size_t observed_seconds{};
        std::array<PredictionHorizonEvaluation, PredictionEvaluationHorizons.size()> horizons{};
    };

    /**
     * @brief Scores forecasts against subsequent observed quote deltas.
     *
     * This component is independent of the prediction algorithm: callers submit
     * cumulative forecasts and the one-second observations used as outcomes.
     */
    class PredictionAnalytics {
    public:
        std::vector<PredictionEvaluationReport> SubmitForecast(PredictionForecast forecast);
        std::vector<PredictionEvaluationReport> ObserveRecord(
            std::int64_t timestamp_ms,
            double mid_delta,
            bool observed = true
        );
        std::vector<PredictionEvaluationReport> FlushIncomplete();

        static std::string FormatReport(const PredictionEvaluationReport& report);

    private:
        struct Observation {
            std::int64_t second{};
            double mid_delta{};
            bool observed{};
        };

        struct PendingForecast {
            PredictionForecast forecast;
            std::array<std::optional<Observation>, PredictionEvaluationWindow> future{};
        };

        static std::int64_t ToSecond(std::int64_t timestamp_ms);
        static PredictionEvaluationReport BuildReport(const PendingForecast& pending, bool window_elapsed);

        std::mutex mutex_;
        std::deque<Observation> history_;
        std::vector<PendingForecast> pending_;
        std::optional<std::int64_t> latest_second_;
    };
}
