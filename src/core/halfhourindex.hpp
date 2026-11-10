// halfhourindex.hpp
// Class and method definitions for all things pertaining to the building and querying of market futures.

#pragma once

#include "structures.hpp"
#include "inputconfig.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <faiss/IndexFlat.h>
#include <faiss/index_io.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

class HalfHourIndex_Test;

namespace Core {

    /**
     * @brief This class represents a single faiss index structure with abilities for load/store and APIs for ANN queries.
     * 
     */
    class HalfHourIndex {
    protected:
        // MEMBERS

        /**
         * @brief Boolean to track if this index has normalization params.
         * 
         */
        bool _hasNormalizationParameters{false};

        /**
         * @brief Number of one-second records in each indexed window.
         * 
         */
        std::size_t _inputWindowSeconds{DEFAULT_INPUT_WINDOW_SECONDS};

        /**
         * @brief Length of each FAISS vector (features per record × window length).
         * 
         */
        int _dimension{static_cast<int>(InputVectorDimension(_inputWindowSeconds))};

        /**
         * @brief Struct to represent the binary-encoded metadata.
         * 
         */
        struct WindowLocation {
            uint32_t recordIndex;
            std::array<char, 8> tradingDay; // "yyyymmdd" and will have to translate -> yyyy-mm-dd.rec
        };

        static_assert(sizeof(WindowLocation) == 12);

        /**
         * @brief Vector of window metadata. The index represents the faiss ID.
         * 
         */
        std::vector<WindowLocation> _windowLocations{};

        /**
         * @brief Underlying FAISS index, sized for this input window.
         */
        faiss::IndexFlatL2 _index;

        /**
         * @brief Boolean to track if the caller made any changes to the structure during the object's lifetime.
         * 
         */
        bool Updated = false;

        /**
         * @brief Parameters describing the standard deviations for this index. Used for normalizing live data.
         * 
         */
        NormalizationParameters _normalizationParameters{};

    private:
        // MEMBERS

        /**
         * @brief Static variable to easily identify num of seconds per half-hour.
         * 
         */
        static constexpr std::size_t SECONDS_PER_HALF_HOUR = 1800;

        /**
         * @brief Describes the total number of minutes in a full trading day.
         * 
         */
        static constexpr std::size_t MARKET_OPEN_MINUTES = 9 * 60 + 30;

        /**
         * @brief Total number of half-hours in a full trading day.
         * 
         */
        static constexpr std::size_t TOTAL_HALF_HOURS = 13;

        /**
         * @brief Stored path of the index file.
         * 
         */
        std::filesystem::path _indexPath;

        /**
         * @brief Stored path of the metadata file.
         * 
         */
        std::filesystem::path _metadataPath;

        /**
         * @brief Path for normalization json metadata.
         * 
         */
        std::filesystem::path _normalizationPath;

        /**
         * @brief Optional index type for index sub-types.
         * 
         */
        std::optional<std::string_view> _indexType{std::nullopt};

        // METHODS

        /**
         * @brief Returns the appropriate index file name based on the number of minutes passed since market start.
         * 
         * @param numOfSeconds Number of seconds that have elapsed since the market opened.
         * @return std::string 
         */
        static std::string GetHalfHourName(std::size_t numOfSeconds);

        /**
         * @brief Gets the file path of the appropraite index file based on the number of minutes passed since market start.
         * 
         * @param numOfSeconds Number of seconds that have elapsed since the market opened.
         * @return std::filesystem::path 
         */
        static std::filesystem::path GetIndexPath(
            std::size_t numOfSeconds,
            std::size_t inputWindowSeconds = DEFAULT_INPUT_WINDOW_SECONDS,
            std::optional<std::string_view> indexType = std::nullopt
        );

        /**
         * @brief Gets the file path of the appropraite metadata file based on the number of minutes passed since market start.
         * 
         * @param numOfSeconds Number of seconds that have elapsed since the market opened.
         * @return std::filesystem::path 
         */
        static std::filesystem::path GetMetadataPath(
            std::size_t numOfSeconds,
            std::size_t inputWindowSeconds = DEFAULT_INPUT_WINDOW_SECONDS,
            std::optional<std::string_view> indexType = std::nullopt
        );

        /**
         * @brief Gets the file path of the "yyyy-mm-dd.rec" file based on the shortened "yyyymmdd" array.
         * 
         * @param yyyymmdd 
         * @return std::filesystem::path 
         */
        static std::filesystem::path GetRecordFilePathFromWindowLocation(const std::array<char,8> &yyyymmdd);

        /**
         * @brief Loads the .meta file from disk into the WindowLocation vector.
         * 
         */
        void LoadMetadata();
        
        /**
         * @brief Saves metadata vector to .meta file on disk, deletes existing.
         * 
         */
        void SaveMetadataToDisk();

        /**
         * @brief Initializes the HalfHourIndex object.
         * 
         */
        void Initialize();

        /**
         * @brief Get the Normalization metadata path.
         * 
         * @param numOfSeconds 
         * @return std::filesystem::path 
         */
        static std::filesystem::path GetNormalizationPath(
            std::size_t numOfSeconds,
            std::size_t inputWindowSeconds = DEFAULT_INPUT_WINDOW_SECONDS,
            std::optional<std::string_view> indexType = std::nullopt
        );

        /**
         * @brief Loads normalization json into struct.
         * 
         */
        void LoadNormalizationParameters();

        /**
         * @brief Saves normalization struct data to json on disk.
         * 
         */
        void SaveNormalizationParametersToDisk() const;

        // FRIENDS AND TEST OVERLOADS
        friend class ::HalfHourIndex_Test;

        /**
         * @brief Construct a new Half Hour Index object for the GTest suite.
         * 
         * @param indexPath 
         * @param metadataPath 
         */
        HalfHourIndex(
            const std::filesystem::path indexPath,
            const std::filesystem::path metadataPath,
            std::size_t inputWindowSeconds = DEFAULT_INPUT_WINDOW_SECONDS,
            std::optional<std::string_view> indexType = std::nullopt
        );

    public:
        // CONSTRUCTOR

        /**
         * @brief Loads a half-hour index for a specific input-window configuration.
         * 
         * @param numOfSeconds Number of seconds since the start of the market day.
         * @param inputWindowSeconds The index dimension size. This also represents the query vector size.
         * @param indexType Type of index this is referring to. See HalfHourMultiIndex for more context.
         */
        HalfHourIndex(const std::size_t &numOfSeconds, std::size_t inputWindowSeconds, std::optional<std::string_view> indexType = std::nullopt);

        // DESTRUCTOR
        virtual ~HalfHourIndex();

        // MEMBERS
        struct QueryResult {
            float distance;
            std::array<char, 8> tradingDayId;
            uint32_t recordIndex;
        };

        /**
         * @brief Constants representing index sub-types. To be used internally for pathing
         * to the appropriate index.
         */
        static constexpr std::string_view INX_TYPE_MAGNITUDE = "m";
        static constexpr std::string_view INX_TYPE_DIRECTION = "d";

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
        virtual void AddVectorsToIndex(
            const std::span<Core::MLRecord> &normalizedMLSpan,
            const std::filesystem::path &recFilePath,
            const std::size_t recordOffset
        ) = 0;

        /**
         * @brief Takes in a normalized query vector and return the specified number of approximate nearest neighbors.
         * Returned in the form of a QueryResult, which describes the particular (.)rec record and the segment's offset.
         * 
         * @param normalizedQuery 
         * @param maxResults 
         * @return std::vector<QueryResult> 
         */
        std::vector<QueryResult> Query(
            const std::vector<float> &normalizedQuery,
            std::size_t maxResults
        ) const;

        /**
         * @brief Checks that the index, metadata, and normalization files for a segment all exist.
         * Constructing a HalfHourIndex without them creates a new, empty index, so read-only
         * callers (like the live runtime) should check first.
         *
         * @param numOfSeconds Any second inside the segment, measured from the market open.
         * @param inputWindowSeconds Input window configuration the index was built for.
         * @return true if all three files are present.
         */
        static bool FilesExist(std::size_t numOfSeconds, std::size_t inputWindowSeconds, std::optional<std::string_view> indexType = std::nullopt);

        /**
        * @brief Sets the normalization parameters associated with this index.
        *
        * Used during historical index construction.
        */
        void SetNormalizationParameters(const NormalizationParameters& parameters);

        /**
        * @brief Normalizes a query of exactly this index's input-window length using its parameters.
        *
        * Returns a flattened FAISS-compatible vector of InputVectorDimension(inputWindowSeconds) floats.
        */
        std::vector<float> NormalizeQuery(std::span<const MLRecord> records) const;

        /**
         * @brief Translates the "storage/records/yyyy-mm-dd.rec" path to yyyymmdd array for storing
         * 
         * @param recFile 
         * @return std::array<char,8> 
         */
        static std::array<char,8> ToShortLocationArray(const std::filesystem::path &recordFilePath);

    };
}
