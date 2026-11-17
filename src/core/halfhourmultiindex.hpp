// halfhourmultiindex.hpp
// Holds the class definition for HalfHourMultiIndex and other definitions.

#pragma once

#include "core/structures.hpp"
#include "halfhourindex.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
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

            /**
             * @param weekdayGroup When set, the index holds only vectors from trading days in this group
             * and is stored apart from the all-week direction index.
             */
            HalfHourIndex_Direction(
                const std::size_t &numOfSeconds,
                std::size_t inputWindowSeconds,
                std::optional<WeekdayGroup> weekdayGroup = std::nullopt
            );

            // METHODS

            /**
            * @brief Takes in a span of MLRecord structs and adds them to the faiss index and calls method(s) to track metadata.
            * We are assuming that the caller has already identified which records belong in this
            * particular time block and are passing them in accordingly.
            * We are also placing them in as-is, so if the data must be normalized, that must occur before this call.
            * A weekday-group index ignores days outside its group; the day is the .rec file's
            * New York session date (see TradingDayWeekday).
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
         * @brief Sub-index containing data for only mid_delta magnitude. Always all-week.
         * Absent only while building, when an existing magnitude index is being reused.
         */
        std::optional<HalfHourIndex_Magnitude> _index_magnitude;

        /**
         * @brief Sub-index containing data for only mid_delta direction, from every trading weekday.
         * 
         */
        std::optional<HalfHourIndex_Direction> _index_direction;

        /**
         * @brief Direction sub-indexes restricted to one weekday group each. The live runtime holds at
         * most one (the current weekday's group); the index builder holds every group it is building.
         */
        std::vector<std::unique_ptr<HalfHourIndex_Direction>> _weekday_direction_indexes;

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
        // MEMBERS

        /**
         * @brief Which sub-indexes a HalfHourMultiIndex holds. The default is the magnitude index plus
         * the all-week direction index: the layout before weekday groups.
         */
        struct SubIndexSelection {
            bool magnitude{true};
            bool all_days_direction{true};
            std::vector<WeekdayGroup> weekday_groups{};

            /**
             * @brief What the live runtime searches in the given mode: the magnitude index, plus the
             * all-week direction index and/or the direction index of weekdayGroup.
             *
             * @throws std::invalid_argument If the mode searches weekday groups and none is given.
             */
            static SubIndexSelection ForSearch(DirectionSearchMode mode, const std::optional<WeekdayGroup> &weekdayGroup);

            /**
             * @brief What "Update Indexes" builds for one segment so the runtime can search it in the
             * given mode: the magnitude index, plus the all-week direction index and/or one direction
             * index per weekday group. When reuseExisting is set, sub-indexes whose files are already
             * complete (and, for weekday groups, whose manifest matches the group) are left out.
             */
            static SubIndexSelection ToBuild(
                std::size_t numOfSeconds,
                std::size_t inputWindowSeconds,
                DirectionSearchMode mode,
                std::span<const WeekdayGroup> weekdayGroups,
                bool reuseExisting
            );

            /**
             * @brief True when no sub-index is selected.
             */
            bool Empty() const;
        };

        // CONSTRUCTORS

        /**
         * @brief Loads a half-hour index for a specific input-window configuration: the magnitude and
         * all-week direction sub-indexes.
         */
        HalfHourMultiIndex(const std::size_t &numOfSeconds, std::size_t inputWindowSeconds);

        /**
         * @brief Loads (or creates, when its files are missing) each selected sub-index of a segment.
         */
        HalfHourMultiIndex(const std::size_t &numOfSeconds, std::size_t inputWindowSeconds, const SubIndexSelection &subIndexes);

        /**
         * @brief Multi-vector result struct containing the query results of both
         * the magnitude index and directional index.
         */
        struct MultiQueryResult {
            std::vector<HalfHourIndex::QueryResult> MagnitudeResults;

            /**
             * @brief Results from the all-week direction index; empty when it is not held.
             */
            std::vector<HalfHourIndex::QueryResult> DirectionResults;

            /**
             * @brief Results from the weekday-group direction index, searched with the same direction
             * query; empty when no weekday group is held. Never mixed with DirectionResults.
             */
            std::vector<HalfHourIndex::QueryResult> WeekdayDirectionResults;
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
         * The direction query is built and temporally weighted once, then searched in the all-week and
         * weekday-group direction indexes that are held. Requires the magnitude index and at most one
         * weekday group.
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

        /**
         * @brief Checks that every selected sub-index of a segment exists on disk (see HalfHourIndex::FilesExist).
         */
        static bool FilesExist(std::size_t numOfSeconds, std::size_t inputWindowSeconds, const SubIndexSelection &subIndexes);

        /**
         * @brief Deletes the files of every selected sub-index of a segment so they can be rebuilt.
         * Sub-indexes that are not selected are left alone.
         */
        static void DeleteFiles(std::size_t numOfSeconds, std::size_t inputWindowSeconds, const SubIndexSelection &subIndexes);

    };

}