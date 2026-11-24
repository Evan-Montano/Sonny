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
        
        /**
         * @brief Accumulates a running mean and population variance without retaining samples.
         */
        struct RunningStatistics {
            // MEMBERS

            std::uint64_t Count{};
            double Mean{};
            double SumSquaredDifferences{};

            // METHODS

            void Add(double value);
            double PopulationStdDev() const;
        };

        RunningStatistics MidDelta{};

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
     * Only the MidDelta feature is normalized.
     */
    void NormalizeRecordsVector(std::span<MLRecord> records, const NormalizationParameters& parameters);

    /**
     * @brief Normalizes records and flattens them into a FAISS-compatible vector.
     *
     * Each record contributes its normalized MidDelta in input order.
     */
    std::vector<float> NormalizeRecordsToVector(std::span<const MLRecord> records, const NormalizationParameters& parameters);

    /**
     * @brief Takes in two quotes from the Webull stream and aggregates them into an MLRecord.
     */
    std::optional<MLRecord> WebullQuoteToMLRecord(const Client::WebullQuote& previousQuote, const Client::WebullQuote& currentQuote);
    
}
