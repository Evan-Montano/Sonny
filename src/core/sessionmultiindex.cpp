// sessionmultiindex.cpp
// Holds the class implementation for SessionMultiIndex.

#include "sessionmultiindex.hpp"
#include "common/logger.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <set>
#include <utility>

namespace Core {

    SessionMultiIndex::SessionMultiIndex(const std::size_t inputWindowSeconds)
        : _inputWindowSeconds(inputWindowSeconds) {

        for (std::size_t segment = 0; segment < SEGMENT_COUNT; ++segment) {
            const std::size_t segmentStartSecond = segment * SECONDS_PER_SEGMENT;

            // Must check before constructing: a missing sub-index would otherwise be created empty and saved on destruction.
            if (HalfHourMultiIndex::FilesExist(segmentStartSecond, _inputWindowSeconds) == false) {
                Logger::Warning(std::format(
                    "No complete index files for the {}s input window in segment {}; that segment is skipped.",
                    _inputWindowSeconds,
                    segment
                ));
                continue;
            }

            try {
                _segments[segment] = std::make_unique<HalfHourMultiIndex>(segmentStartSecond, _inputWindowSeconds);
            }
            catch (const std::exception &error) {
                Logger::Error(
                    std::format("Failed to load the {}s input index for segment {}: {}", _inputWindowSeconds, segment, error.what()),
                    true
                );
            }
        }

        // The common magnitude scale depends on which segments actually loaded.
        std::vector<NormalizationParameters> loadedParameters{};

        for (const std::unique_ptr<HalfHourMultiIndex> &segment : _segments) {
            if (segment != nullptr) {
                loadedParameters.push_back(segment->GetNormalizationParameters());
            }
        }

        const double referenceStdDev = PooledStandardDeviation(loadedParameters);

        for (std::size_t segment = 0; segment < SEGMENT_COUNT; ++segment) {
            if (_segments[segment] == nullptr) {
                continue;
            }

            _magnitudeDistanceScales[segment] = MagnitudeDistanceScale(
                _segments[segment]->GetNormalizationParameters().mid_delta_stddev,
                referenceStdDev
            );

            if (_magnitudeDistanceScales[segment] > 0.0 == false) {
                Logger::Warning(std::format(
                    "The {}s input magnitude index for segment {} has no usable standard deviation; "
                    "its magnitude matches are left out.",
                    _inputWindowSeconds,
                    segment
                ));
            }
        }
    }

    std::size_t SessionMultiIndex::LoadedSegmentCount() const {
        return static_cast<std::size_t>(
            std::count_if(_segments.begin(), _segments.end(), [](const auto &segment) { return segment != nullptr; })
        );
    }

    HalfHourMultiIndex::MultiQueryResult SessionMultiIndex::Query(
        const std::span<const MLRecord> query,
        const std::size_t maxResults
    ) const {
        // Magnitude and direction candidates never share a collection.
        std::vector<HalfHourIndex::QueryResult> magnitudeCandidates{};
        std::vector<HalfHourIndex::QueryResult> directionCandidates{};

        for (std::size_t segment = 0; segment < SEGMENT_COUNT; ++segment) {
            if (_segments[segment] == nullptr) {
                continue;
            }

            HalfHourMultiIndex::MultiQueryResult segmentResults = _segments[segment]->Query(query, maxResults);

            // A positive factor keeps the segment's own nearest-first order, so the segment's
            // nearest maxResults are still its nearest maxResults on the common scale.
            const double magnitudeScale = _magnitudeDistanceScales[segment];

            if (magnitudeScale > 0.0) {
                for (HalfHourIndex::QueryResult &result : segmentResults.MagnitudeResults) {
                    result.distance = static_cast<float>(static_cast<double>(result.distance) * magnitudeScale);
                    magnitudeCandidates.push_back(result);
                }
            }

            // Direction vectors are raw {-1, 0, 1} in every segment, so their distances already agree.
            directionCandidates.insert(
                directionCandidates.end(),
                segmentResults.DirectionResults.begin(),
                segmentResults.DirectionResults.end()
            );
        }

        return HalfHourMultiIndex::MultiQueryResult{
            .MagnitudeResults = SelectNearest(std::move(magnitudeCandidates), maxResults),
            .DirectionResults = SelectNearest(std::move(directionCandidates), maxResults)
        };
    }

    std::vector<HalfHourIndex::QueryResult> SessionMultiIndex::SelectNearest(
        std::vector<HalfHourIndex::QueryResult> candidates,
        const std::size_t maxResults
    ) {
        // A NaN distance has no place in an ordering and could never become a match anyway.
        std::erase_if(candidates, [](const HalfHourIndex::QueryResult &candidate) {
            return std::isfinite(candidate.distance) == false;
        });

        // "yyyymmdd" compares in date order, so the larger trading day is the newer one.
        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const HalfHourIndex::QueryResult &left, const HalfHourIndex::QueryResult &right) {
                if (left.distance != right.distance) return left.distance < right.distance;
                if (left.tradingDayId != right.tradingDayId) return left.tradingDayId > right.tradingDayId;
                return left.recordIndex < right.recordIndex;
            }
        );

        std::vector<HalfHourIndex::QueryResult> selected{};
        selected.reserve(std::min(maxResults, candidates.size()));

        // Candidates are nearest first, so the first sighting of a window carries its smallest distance.
        std::set<std::pair<std::array<char, 8>, uint32_t>> seenWindows{};

        for (const HalfHourIndex::QueryResult &candidate : candidates) {
            if (selected.size() >= maxResults) {
                break;
            }

            if (seenWindows.emplace(candidate.tradingDayId, candidate.recordIndex).second) {
                selected.push_back(candidate);
            }
        }

        return selected;
    }

    double SessionMultiIndex::PooledStandardDeviation(const std::span<const NormalizationParameters> parameters) {
        double sampleCount = 0.0;
        double sum = 0.0;
        double sumOfSquares = 0.0;

        for (const NormalizationParameters &segment : parameters) {
            const double count = static_cast<double>(segment.sample_count);

            sampleCount += count;
            sum += count * segment.mid_delta_mean;
            sumOfSquares += count * (
                segment.mid_delta_stddev * segment.mid_delta_stddev +
                segment.mid_delta_mean * segment.mid_delta_mean
            );
        }

        double result = 0.0;

        if (sampleCount > 0.0) {
            const double mean = sum / sampleCount;

            // Rounding can leave a tiny negative variance when every segment is identical.
            result = std::sqrt(std::max(sumOfSquares / sampleCount - mean * mean, 0.0));
        }

        return result;
    }

    double SessionMultiIndex::MagnitudeDistanceScale(const double segmentStdDev, const double referenceStdDev) {
        double scale = 0.0;

        if (segmentStdDev > 0.0 && referenceStdDev > 0.0) {
            const double ratio = segmentStdDev / referenceStdDev;
            scale = ratio * ratio;
        }

        return scale;
    }

}
