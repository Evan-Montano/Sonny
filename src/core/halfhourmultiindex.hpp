// halfhourmultiindex.hpp
// Holds the class definition for HalfHourMultiIndex and other definitions.

#pragma once

#include "core/structures.hpp"
#include "halfhourindex.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace Core {

    /**
     * @brief This is an orchestrating class for derived half hour indexes.
     * Since multiple prediction models are being used, this class handles the
     * implementation.
     */
    class HalfHourMultiIndex {
    private:
        // SUB-INDEX CLASSES

        /**
         * @brief Child class of HalfHourIndex. The information building this
         * index is the ABSOLUTE MAGNITUDE of mid_delta changes in a one-second
         * interval.
         */
        class HalfHourIndex_Magnitude : public HalfHourIndex {
        private:

        public:
            // CONSTRUCTORS

            HalfHourIndex_Magnitude(const std::size_t &numOfSeconds, std::size_t inputWindowSeconds);

            /**
            * @brief Takes in a span of MLRecord structs and adds them to the faiss index and calls method(s) to track metadata.
            * We are assuming that the caller has already identified which records belong in this
            * particular time block and are passing them in accordingly.
            * We are also placing them in as-is, so if the data must be normalized, that must occur before this call.
            * @param normalizedMLSpan 
            * @param recFilePath 
            * @param recordOffset 
            */
            void AddVectorsToIndex(
                const std::span<Core::MLRecord> &segment,
                const std::filesystem::path &recFilePath,
                const std::size_t recordOffset
            ) override;

            /**
             * @brief Public accessor to check if normalization params are set.
             * 
             * @return true 
             * @return false 
             */
            bool HasNormalizationParameters();

            /**
             * @brief Public read access to the parameters this index's vectors were normalized with.
             */
            const NormalizationParameters &GetNormalizationParameters() const;

        };

        /**
         * @brief Child class of HalfHourIndex. The information building this
         * index is the DIRECTION of mid_delta changes in a one-second
         * interval.
         */
        class HalfHourIndex_Direction : public HalfHourIndex {
        private:

        public:
            // CONSTRUCTORS

            HalfHourIndex_Direction(const std::size_t &numOfSeconds, std::size_t inputWindowSeconds);

            // METHODS

            /**
            * @brief Takes in a span of MLRecord structs and adds them to the faiss index and calls method(s) to track metadata.
            * We are assuming that the caller has already identified which records belong in this
            * particular time block and are passing them in accordingly.
            * We are also placing them in as-is, so if the data must be normalized, that must occur before this call.
            * @param normalizedMLSpan 
            * @param recFilePath 
            * @param recordOffset 
            */
            void AddVectorsToIndex(
                const std::span<Core::MLRecord> &normalizedMLSpan,
                const std::filesystem::path &recFilePath,
                const std::size_t recordOffset
            ) override;
            
        };

        // MEMBERS

        /**
         * @brief Sub-index containing data for only mid_delta magnitude.
         * 
         */
        HalfHourIndex_Magnitude _index_magnitude;

        /**
         * @brief Sub-index containing data for only mid_delta direction.
         * 
         */
        HalfHourIndex_Direction _index_direction;

        /**
         * @brief Number of one-second records in each indexed window.
         * 
         */
        std::size_t _inputWindowSeconds{DEFAULT_INPUT_WINDOW_SECONDS};

        struct MultiQuery {
            std::vector<float> MagnitudeQuery;
            std::vector<float> DirectionQuery;
        };

        // METHODS

        /**
         * @brief Takes in the raw unnormalized quote deltas, then splits the vector into
         * two new vectors for magnitude and direction, then normalizes the magnitude.
         * 
         * @param records 
         * @return MultiQuery 
         */
        MultiQuery CreateMultiQuery(std::span<const MLRecord> records) const;


    public:
        // CONSTRUCTORS

        /**
         * @brief Loads a half-hour index for a specific input-window configuration.
         */
        HalfHourMultiIndex(const std::size_t &numOfSeconds, std::size_t inputWindowSeconds);

        // MEMBERS

        /**
         * @brief Multi-vector result struct containing the query results of both
         * the magnitude index and directional index.
         */
        struct MultiQueryResult {
            std::vector<HalfHourIndex::QueryResult> MagnitudeResults;
            std::vector<HalfHourIndex::QueryResult> DirectionResults;
        };

        // METHODS

        /**
        * @brief Sets the normalization parameters associated with this index.
        *
        * Used during historical index construction.
        */
        void SetNormalizationParameters(const NormalizationParameters &parameters);

        /**
         * @brief Returns the normalization parameters of the magnitude sub-index.
         *
         * Each segment is normalized on its own, so its magnitude distances are in units of its own
         * standard deviation. SessionMultiIndex uses these to put every segment on one scale.
         */
        const NormalizationParameters &GetNormalizationParameters() const;

        /**
         * @brief Takes in a span of MLRecord structs and adds them to the faiss index and calls method(s) to track metadata.
         * We are assuming that the caller has already identified which records belong in this
         * particular time block and are passing them in accordingly.
         * @param normalizedMLSpan
         * @param recFilePath 
         * @param recordOffset 
         */
        void AddVectorsToIndex(
            const std::span<Core::MLRecord> &segment,
            const std::filesystem::path &recFilePath,
            const std::size_t recordOffset
        );

        /**
         * @brief Takes in a raw, unnormalized query vector and returns a struct the specified number of
         * approximate nearest neighbors for both sub-index types.
         *
         * Results are ordered by distance; ties go to the vector added first. Setup adds days newest
         * first, so tied matches (common in the direction index) prefer the most recent days.
         * 
         * @param normalizedQuery 
         * @param maxResults 
         * @return MultiQueryResult
         */
        MultiQueryResult Query(
            std::span<const MLRecord> query,
            std::size_t maxResults
        ) const;

        /**
         * @brief Checks that both sub-indexes for a segment exist on disk. The magnitude index needs its
         * index, metadata, and normalization files; the direction index has no normalization file.
         * Constructing a HalfHourMultiIndex without them creates new, empty indexes that are written
         * back to disk on destruction, so read-only callers (like the live runtime) must check first.
         *
         * @param numOfSeconds Any second inside the segment, measured from the market open.
         * @param inputWindowSeconds Input window configuration the indexes were built for.
         * @return true if every required file is present.
         */
        static bool FilesExist(std::size_t numOfSeconds, std::size_t inputWindowSeconds);

    };

}