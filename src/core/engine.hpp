// engine.hpp
// Class and method definitions for all things pertaining to the building and querying of market futures.

#pragma once

#include "structures.hpp"

#include <array>
#include <cstddef>
#include <faiss/IndexFlat.h>
#include <faiss/index_io.h>

#include <filesystem>
#include <span>

namespace Core {

    /**
     * @brief This class represents a single faiss index structure with abilities for load/store and APIs for ANN queries.
     * 
     */
    class HalfHourIndex {
    private:
        // MEMBERS

        /**
         * @brief Dimension of index vectors. This is calculated by multiplying the
         * number of parameters in MLRecord struct(3): mid_delta, spread_delta, and volume
         * by the target number of seconds we are using as a context window for
         * market future predictions, which is 30s.
         */
        static constexpr int DIMENSION = 90;

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
         * @brief Underlying faiss index.
         * 
         */
        faiss::IndexFlatL2 _index;

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
         * @brief Boolean to track if the caller made any changes to the structure during the object's lifetime.
         * 
         */
        bool Updated = false;

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

        // METHODS

        /**
         * @brief Returns the appropriate index file name based on the number of minutes passed since market start.
         * 
         * @param numOfMinutes 
         * @return std::string 
         */
        static std::string GetHalfHourName(std::size_t numOfMinutes);

        /**
         * @brief Gets the file path of the appropraite index file based on the number of minutes passed since market start.
         * 
         * @param numOfMinutes 
         * @return std::filesystem::path 
         */
        static std::filesystem::path GetIndexPath(std::size_t numOfMinutes);

        /**
         * @brief Gets the file path of the appropraite metadata file based on the number of minutes passed since market start.
         * 
         * @param numOfMinutes 
         * @return std::filesystem::path 
         */
        static std::filesystem::path GetMetadataPath(std::size_t numOfMinutes);

        /**
         * @brief Gets the file path of the "yyyy-mm-dd.rec" file based on the shortened "yyyymmdd" array.
         * 
         * @param yyyymmdd 
         * @return std::filesystem::path 
         */
        static std::filesystem::path GetRecordFilePathFromWindowLocation(const std::array<char,8> &yyyymmdd);

        /**
         * @brief Translates the "storage/records/yyyy-mm-dd.rec" path to yyyymmdd array for storing
         * 
         * @param recFile 
         * @return std::array<char,8> 
         */
        static std::array<char,8> ToShortLocationArray(const std::filesystem::path &recordFilePath);

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

    public:
        // CONSTRUCTOR

        /**
         * @brief Construct a new Half Hour Index object.
         * Calling an empty constructor will initialize an empty faiss index.
         */
        HalfHourIndex()
            : _index(DIMENSION) {

        }

        /**
         * @brief Construct a new Half Hour Index object.
         * Calling this constructor will find the appropriate, existing
         * index structure on disk in "~/storage/indexes/" and load it in.
         * @param numOfMinutes Number of minutes that have passed since the market open.
         */
        HalfHourIndex(const std::size_t &numOfMinutes);

        // DESTRUCTOR
        ~HalfHourIndex();

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
        );
    };

}