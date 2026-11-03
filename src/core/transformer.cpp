// transformer.cpp
// Contains method implementations for things pertaining to data manipulation or normalization in bulk.

#include "transformer.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace Core {

    // ========== NORMALIZATION ACCUMULATOR ========== //

    void NormalizationAccumulator::RunningStatistics::Add(double value) {
        ++count;

        const double delta = value - mean;

        mean += delta / static_cast<double>(count);

        const double deltaAfterMeanUpdate = value - mean;

        sumSquaredDifferences += delta * deltaAfterMeanUpdate;
    }

    double NormalizationAccumulator::RunningStatistics::PopulationStdDev() const {
        double result = 0.0;

        if (count > 0) {
            result = std::sqrt(
                sumSquaredDifferences / static_cast<double>(count)
            );
        }

        return result;
    }

    void NormalizationAccumulator::AddRecord(const MLRecord& record) {
        const bool valid =
            std::isfinite(record.mid_delta) &&
            std::isfinite(record.spread_delta) &&
            std::isfinite(record.volume) &&
            record.volume >= 0.0f;

        if (valid == false) {
            throw std::invalid_argument(
                "Cannot accumulate normalization statistics from an invalid MLRecord."
            );
        }

        midDelta.Add(record.mid_delta);
        spreadDelta.Add(record.spread_delta);
        volume.Add(std::log1p(static_cast<double>(record.volume)));
    }

    void NormalizationAccumulator::AddRecords(std::span<const MLRecord> records) {
        for (const MLRecord& record : records) {
            AddRecord(record);
        }
    }

    NormalizationParameters NormalizationAccumulator::GetParameters() const {
        if (midDelta.count == 0) {
            throw std::runtime_error(
                "Cannot calculate normalization parameters without records."
            );
        }

        return NormalizationParameters{
            .sample_count = midDelta.count,

            .mid_delta_mean = midDelta.mean,
            .mid_delta_stddev = midDelta.PopulationStdDev(),

            .spread_delta_mean = spreadDelta.mean,
            .spread_delta_stddev = spreadDelta.PopulationStdDev(),

            .volume_mean = volume.mean,
            .volume_stddev = volume.PopulationStdDev()
        };
    }

    // ========== END NORMALIZATION ACCUMULATOR ========== //


    // ========== NORMALIZATION VALIDATION ========== //

    void ValidateNormalizationParameters(const NormalizationParameters& parameters) {
        const bool valid =
            parameters.sample_count > 0 &&
            std::isfinite(parameters.mid_delta_mean) &&
            std::isfinite(parameters.mid_delta_stddev) &&
            std::isfinite(parameters.spread_delta_mean) &&
            std::isfinite(parameters.spread_delta_stddev) &&
            std::isfinite(parameters.volume_mean) &&
            std::isfinite(parameters.volume_stddev) &&
            parameters.mid_delta_stddev >= 0.0 &&
            parameters.spread_delta_stddev >= 0.0 &&
            parameters.volume_stddev >= 0.0;

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
            if (std::isfinite(record.volume) == false ||
                record.volume < 0.0f) {
                throw std::invalid_argument(
                    "Cannot normalize an invalid volume value."
                );
            }

            const double logVolume = std::log1p(static_cast<double>(record.volume));

            return MLRecord{
                .mid_delta = NormalizeFeature(
                    record.mid_delta,
                    parameters.mid_delta_mean,
                    parameters.mid_delta_stddev
                ),

                .spread_delta = NormalizeFeature(
                    record.spread_delta,
                    parameters.spread_delta_mean,
                    parameters.spread_delta_stddev
                ),

                .volume = NormalizeFeature(
                    logVolume,
                    parameters.volume_mean,
                    parameters.volume_stddev
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
        result.reserve(records.size() * 3);

        for (const MLRecord& record : records) {
            const MLRecord normalized = NormalizeRecord(
                record,
                parameters
            );

            result.push_back(normalized.mid_delta);
            result.push_back(normalized.spread_delta);
            result.push_back(normalized.volume);
        }

        return result;
    }

    // ========== END NORMALIZATION IMPLEMENTATION ========== //


    // ========== WEBULL QUOTE CONVERSION ========== //

    std::optional<MLRecord> WebullQuoteToMLRecord(const Client::WebullQuote &previousQuote, const Client::WebullQuote &currentQuote) {
        std::optional<MLRecord> res;

        bool valid =
            previousQuote.asks.empty() == false &&
            previousQuote.bids.empty() == false &&
            currentQuote.asks.empty() == false &&
            currentQuote.bids.empty() == false;

        if (valid) {
            const Client::WebullPriceLevel &previousAsk = previousQuote.asks.front();
            const Client::WebullPriceLevel &previousBid = previousQuote.bids.front();
            const Client::WebullPriceLevel &currentAsk = currentQuote.asks.front();
            const Client::WebullPriceLevel &currentBid = currentQuote.bids.front();

            const double previousMid = (previousAsk.Price + previousBid.Price) / 2.0;
            const double currentMid = (currentAsk.Price + currentBid.Price) / 2.0;
            const double previousSpread = previousAsk.Price - previousBid.Price;
            const double currentSpread = currentAsk.Price - currentBid.Price;

            // Experimental volume proxy: average displayed size
            // at the best bid and ask.
            const double volume = (currentAsk.size + currentBid.size) / 2.0;
            const double midDelta = currentMid - previousMid;
            const double spreadDelta = currentSpread - previousSpread;

            valid =
                std::isfinite(midDelta) &&
                std::isfinite(spreadDelta) &&
                std::isfinite(volume);
            
            if (valid) {
                res = MLRecord{
                    .mid_delta = static_cast<float>(midDelta),
                    .spread_delta = static_cast<float>(spreadDelta),
                    .volume = static_cast<float>(volume)
                };
            }

        }

        return res;
    }

}