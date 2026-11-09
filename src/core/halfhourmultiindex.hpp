// halfhourmultiindex.hpp
// Holds the class definition for HalfHourMultiIndex and other definitions.

#include "core/structures.hpp"
#include "halfhourindex.hpp"

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
                const std::span<Core::MLRecord> &normalizedMLSpan,
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


    public:
        // CONSTRUCTORS

        /**
         * @brief Loads a half-hour index for a specific input-window configuration.
         */
        HalfHourMultiIndex(const std::size_t &numOfSeconds, std::size_t inputWindowSeconds);

        // METHODS

        /**
        * @brief Sets the normalization parameters associated with this index.
        *
        * Used during historical index construction.
        */
        void SetNormalizationParameters(const NormalizationParameters &parameters);

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
        );

        /**
         * @brief Takes in a normalized query vector and returns the specified number of approximate nearest neighbors.
         * Returning the actual predicted futures, unlike the Query method in base HalfHourIndex, which only 
         * returns QueryResult structs.
         * 
         * @param normalizedQuery 
         * @param maxResults 
         * @return std::vector<QueryResult> 
         */
        std::vector<std::vector<float>> Query(
            const std::vector<float> &normalizedQuery,
            std::size_t maxResults
        ) const;

    };

}