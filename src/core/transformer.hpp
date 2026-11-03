// transformer.hpp
// Contains method definitions for things pertaining to data manipulation or normalization in bulk.

#pragma once

#include "client/webullstream.hpp"
#include "structures.hpp"

#include <span>
#include <vector>

namespace Core {


    /**
     * @brief Accumulates statistics across multiple MLRecord collections.
     *
     * Uses Welford's online algorithm to calculate population statistics
     * without storing the original records.
     */
    class NormalizationAccumulator {
    private:
        // MEMBERS
        
        struct RunningStatistics {
            std::uint64_t count{};
            double mean{};
            double sumSquaredDifferences{};

            void Add(double value);
            double PopulationStdDev() const;
        };

        RunningStatistics midDelta{};
        RunningStatistics spreadDelta{};
        RunningStatistics volume{};

    public:
        // METHODS

        void AddRecord(const MLRecord& record);

        void AddRecords(std::span<const MLRecord> records);

        NormalizationParameters GetParameters() const;
    };

    /**
     * @brief Validates a set of normalization parameters.
     *
     * @throws std::invalid_argument If the parameters are invalid.
     */
    void ValidateNormalizationParameters(const NormalizationParameters& parameters);

    /**
     * @brief Normalizes records in place using supplied parameters.
     *
     * Volume is log-transformed before normalization.
     */
    void NormalizeRecordsVector(std::span<MLRecord> records, const NormalizationParameters& parameters);

    /**
     * @brief Normalizes records and flattens them into a FAISS-compatible vector.
     *
     * Feature ordering:
     * mid_delta, spread_delta, volume.
     */
    std::vector<float> NormalizeRecordsToVector(std::span<const MLRecord> records, const NormalizationParameters& parameters);

    /**
     * @brief Takes in two quotes from the Webull stream and aggregates them into an MLRecord.
     */
    std::optional<MLRecord> WebullQuoteToMLRecord(const Client::WebullQuote& previousQuote, const Client::WebullQuote& currentQuote);
    
}