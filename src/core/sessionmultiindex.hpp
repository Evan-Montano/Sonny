// sessionmultiindex.hpp
// Holds the class definition for SessionMultiIndex: every half-hour segment of one input window, searched as one.

#pragma once

#include "halfhourindex.hpp"
#include "halfhourmultiindex.hpp"
#include "structures.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace Core {

    /**
     * @brief Loads the HalfHourMultiIndex of every half-hour segment for one input window and answers
     * a query with the nearest matches across the whole session.
     *
     * Each segment is only a source of candidates. They are pooled, ranked together, and capped once,
     * so the result is the nearest matches overall rather than the nearest per segment.
     * Magnitude and direction candidates are pooled, ranked, and capped separately throughout.
     */
    class SessionMultiIndex {
    public:
        // MEMBERS

        /**
         * @brief Number of half-hour segments in the regular session (09:30-16:00 ET).
         */
        static constexpr std::size_t SEGMENT_COUNT = 13;

        /**
         * @brief Number of session seconds covered by one segment.
         */
        static constexpr std::size_t SECONDS_PER_SEGMENT = 1800;

        // CONSTRUCTOR

        /**
         * @brief Loads every segment that has complete index files for the given input window.
         *
         * A segment whose files are missing is skipped with a warning, and one that fails to load is
         * skipped with an error; the remaining segments are still searched. Nothing is written to disk.
         *
         * @param inputWindowSeconds Input window the indexes were built for.
         */
        explicit SessionMultiIndex(std::size_t inputWindowSeconds);

        // METHODS

        /**
         * @brief Number of segments that loaded and take part in queries.
         */
        std::size_t LoadedSegmentCount() const;

        /**
         * @brief Queries every loaded segment and returns the nearest matches across all of them.
         *
         * Up to maxResults candidates are taken from each segment, pooled per sub-index, reduced with
         * SelectNearest, and capped at maxResults overall. Magnitude distances are first converted
         * to one common scale (see MagnitudeDistanceScale); direction distances need no conversion.
         *
         * @param query Raw, unnormalized window of exactly the configured input-window length.
         * @param maxResults Cap applied to each segment's candidates and again to the pooled result.
         * @return Magnitude and direction results, each nearest first.
         */
        HalfHourMultiIndex::MultiQueryResult Query(
            std::span<const MLRecord> query,
            std::size_t maxResults
        ) const;

        /**
         * @brief Reduces pooled candidates to the nearest maxResults distinct historical windows.
         *
         * Order is by distance, then newest trading day, then earliest record, so the result does not
         * depend on the order the segments were searched in and tied matches prefer the most recent
         * days. This is the same order a single index returns ties in, because Setup adds days newest
         * first and FAISS breaks ties by the lowest vector ID; the two must stay in step, or a segment
         * could cut off tied candidates that the pooled ranking would have preferred.
         * A window that appears more than once (same trading day and record index) is kept once,
         * with its smallest distance. Candidates without a finite distance are dropped.
         *
         * @param candidates Candidates from any number of segments, in any order.
         * @param maxResults Most results to return.
         * @return The selected candidates, nearest first.
         */
        static std::vector<HalfHourIndex::QueryResult> SelectNearest(
            std::vector<HalfHourIndex::QueryResult> candidates,
            std::size_t maxResults
        );

        /**
         * @brief Standard deviation the segments would share if they had been normalized together.
         * Used as the common unit for magnitude distances.
         *
         * @param parameters Normalization parameters of each loaded segment.
         * @return The pooled population standard deviation, or 0 if there are no samples.
         */
        static double PooledStandardDeviation(std::span<const NormalizationParameters> parameters);

        /**
         * @brief Factor that converts a segment's magnitude distance to the common scale.
         *
         * A segment stores (|mid_delta| - mean) / stddev using its own statistics. The mean cancels
         * out of a distance, so a segment's squared distance is the raw squared distance divided by
         * its stddev^2. Segments with larger price moves (the open and close) therefore report
         * smaller distances for the same raw difference, and their distances cannot be compared with
         * another segment's as they are. Multiplying by (segmentStdDev / referenceStdDev)^2 puts
         * every segment in units of the reference instead.
         *
         * @return The factor, or 0 when either standard deviation is not positive (not comparable).
         */
        static double MagnitudeDistanceScale(double segmentStdDev, double referenceStdDev);

    private:
        // MEMBERS

        /**
         * @brief Number of one-second records in each indexed window.
         */
        std::size_t _inputWindowSeconds;

        /**
         * @brief One entry per segment in session order; nullptr where the segment is unavailable.
         */
        std::array<std::unique_ptr<HalfHourMultiIndex>, SEGMENT_COUNT> _segments{};

        /**
         * @brief MagnitudeDistanceScale of each segment; 0 where the segment is unavailable or
         * its magnitude distances cannot be compared.
         */
        std::array<double, SEGMENT_COUNT> _magnitudeDistanceScales{};
    };

}
