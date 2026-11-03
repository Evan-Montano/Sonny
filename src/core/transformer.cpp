// transformer.cpp
// Contains method implementations for things pertaining to data manipulation or normalization in bulk.

#include "transformer.hpp"

#include <cmath>

namespace Core {

    void NormalizeRecordsVector(std::vector<MLRecord>& records) {
        if (records.empty() == false) {
            double midSum = 0.0;
            double spreadSum = 0.0;
            double volumeSum = 0.0;

            // Calculate the mean of each feature.
            // Volume is log-transformed first to reduce the effect of
            // extremely large volume spikes.
            for (const MLRecord& record : records) {
                midSum += record.mid_delta;
                spreadSum += record.spread_delta;
                volumeSum += std::log1p(record.volume);
            }

            const double count = static_cast<double>(records.size());

            const double midMean = midSum / count;
            const double spreadMean = spreadSum / count;
            const double volumeMean = volumeSum / count;

            double midSquaredDiffSum = 0.0;
            double spreadSquaredDiffSum = 0.0;
            double volumeSquaredDiffSum = 0.0;

            // Calculate the variance of each feature.
            for (const MLRecord& record : records) {
                const double midDiff = record.mid_delta - midMean;
                const double spreadDiff = record.spread_delta - spreadMean;

                const double logVolume = std::log1p(record.volume);
                const double volumeDiff = logVolume - volumeMean;

                midSquaredDiffSum += midDiff * midDiff;
                spreadSquaredDiffSum += spreadDiff * spreadDiff;
                volumeSquaredDiffSum += volumeDiff * volumeDiff;
            }

            const double midStdDev = std::sqrt(midSquaredDiffSum / count);
            const double spreadStdDev = std::sqrt(spreadSquaredDiffSum / count);
            const double volumeStdDev = std::sqrt(volumeSquaredDiffSum / count);

            // Normalize every feature in place.
            for (MLRecord& record : records) {
                // Mid delta
                if (midStdDev > 0.0) {
                    record.mid_delta = static_cast<float>(
                        (record.mid_delta - midMean) / midStdDev
                    );
                }
                else {
                    record.mid_delta = 0.0f;
                }

                // Spread delta
                if (spreadStdDev > 0.0) {
                    record.spread_delta = static_cast<float>(
                        (record.spread_delta - spreadMean) / spreadStdDev
                    );
                }
                else {
                    record.spread_delta = 0.0f;
                }

                // Volume
                // Log-transform first, then z-score normalize.
                if (volumeStdDev > 0.0) {
                    const double logVolume = std::log1p(record.volume);

                    record.volume = static_cast<float>(
                        (logVolume - volumeMean) / volumeStdDev
                    );
                }
                else {
                    record.volume = 0.0f;
                }
            }
        }
    }

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