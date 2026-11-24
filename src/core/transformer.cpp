// transformer.cpp
// Contains method implementations for things pertaining to data manipulation or normalization in bulk.

#include "transformer.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace Core {

    // ========== NORMALIZATION ACCUMULATOR ========== //

    void NormalizationAccumulator::RunningStatistics::Add(double value) {
        ++Count;

        const double delta = value - Mean;

        Mean += delta / static_cast<double>(Count);

        const double deltaAfterMeanUpdate = value - Mean;

        SumSquaredDifferences += delta * deltaAfterMeanUpdate;
    }

    double NormalizationAccumulator::RunningStatistics::PopulationStdDev() const {
        double result = 0.0;

        if (Count > 0) {
            result = std::sqrt(
                SumSquaredDifferences / static_cast<double>(Count)
            );
        }

        return result;
    }

    void NormalizationAccumulator::AddRecord(const MLRecord& record) {
        const bool valid = std::isfinite(record.MidDelta);

        if (valid == false) {
            throw std::invalid_argument(
                "Cannot accumulate normalization statistics from an invalid MLRecord."
            );
        }

        MidDelta.Add(record.MidDelta);
    }

    void NormalizationAccumulator::AddRecords(std::span<const MLRecord> records) {
        for (const MLRecord& record : records) {
            AddRecord(record);
        }
    }

    NormalizationParameters NormalizationAccumulator::GetParameters() const {
        if (MidDelta.Count == 0) {
            throw std::runtime_error(
                "Cannot calculate normalization parameters without records."
            );
        }

        return NormalizationParameters{
            .SampleCount = MidDelta.Count,

            .MidDeltaMean = MidDelta.Mean,
            .MidDeltaStddev = MidDelta.PopulationStdDev()
        };
    }

    // ========== END NORMALIZATION ACCUMULATOR ========== //


    // ========== NORMALIZATION VALIDATION ========== //

    void ValidateNormalizationParameters(const NormalizationParameters& parameters) {
        const bool valid =
            parameters.SampleCount > 0 &&
            std::isfinite(parameters.MidDeltaMean) &&
            std::isfinite(parameters.MidDeltaStddev) &&
            parameters.MidDeltaStddev >= 0.0;

        if (valid == false) {
            throw std::invalid_argument(
                "Invalid normalization parameters."
            );
        }
    }

    // ========== END NORMALIZATION VALIDATION ========== //


    // ========== NORMALIZATION IMPLEMENTATION ========== //

    namespace {

        float NormalizeFeature(double value, double mean, double stddev) {
            double normalized = 0.0;

            if (std::isfinite(value) == false) {
                throw std::invalid_argument(
                    "Cannot normalize a non-finite feature value."
                );
            }

            if (stddev > 0.0) {
                normalized = (value - mean) / stddev;
            }

            if (std::isfinite(normalized) == false ||
                std::abs(normalized) > std::numeric_limits<float>::max()) {
                throw std::overflow_error(
                    "Normalized feature exceeds the float range."
                );
            }

            return static_cast<float>(normalized);
        }

        MLRecord NormalizeRecord(const MLRecord& record, const NormalizationParameters& parameters) {
            return MLRecord{
                .MidDelta = NormalizeFeature(
                    record.MidDelta,
                    parameters.MidDeltaMean,
                    parameters.MidDeltaStddev
                )
            };
        }

    }

    void NormalizeRecordsVector(std::span<MLRecord> records, const NormalizationParameters& parameters) {
        ValidateNormalizationParameters(parameters);

        for (MLRecord& record : records) {
            record = NormalizeRecord(record, parameters);
        }
    }

    std::vector<float> NormalizeRecordsToVector(std::span<const MLRecord> records, const NormalizationParameters& parameters) {
        ValidateNormalizationParameters(parameters);

        std::vector<float> result;
        result.reserve(records.size() * 2);

        for (const MLRecord& record : records) {
            const MLRecord normalized = NormalizeRecord(
                record,
                parameters
            );

            result.push_back(normalized.MidDelta);
        }

        return result;
    }

    // ========== END NORMALIZATION IMPLEMENTATION ========== //


    // ========== WEBULL QUOTE CONVERSION ========== //

    std::optional<MLRecord> WebullQuoteToMLRecord(const Client::WebullQuote &previousQuote, const Client::WebullQuote &currentQuote) {
        std::optional<MLRecord> res;

        bool valid =
            previousQuote.Asks.empty() == false &&
            previousQuote.Bids.empty() == false &&
            currentQuote.Asks.empty() == false &&
            currentQuote.Bids.empty() == false;

        if (valid) {
            const Client::WebullPriceLevel &previousAsk = previousQuote.Asks.front();
            const Client::WebullPriceLevel &previousBid = previousQuote.Bids.front();
            const Client::WebullPriceLevel &currentAsk = currentQuote.Asks.front();
            const Client::WebullPriceLevel &currentBid = currentQuote.Bids.front();

            const double previousMid = (previousAsk.Price + previousBid.Price) / 2.0;
            const double currentMid = (currentAsk.Price + currentBid.Price) / 2.0;

            const double midDelta = currentMid - previousMid;

            valid = 
            std::isfinite(midDelta);
            
            if (valid) {
                res = MLRecord{
                    .MidDelta = static_cast<float>(midDelta)
                };
            }

        }

        return res;
    }

}
