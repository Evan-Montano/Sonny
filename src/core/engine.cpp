// engine.cpp
// Class and method implementations for all things pertaining to the building and querying of market futures.

#include "engine.hpp"
#include "../common/storage.hpp"
#include "../common/logger.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <stdexcept>


namespace Core {

    std::string HalfHourIndex::GetHalfHourName(const std::size_t numOfSeconds) {
        const std::size_t bucket = numOfSeconds / SECONDS_PER_HALF_HOUR;
        
        if (bucket >= TOTAL_HALF_HOURS) {
            throw std::out_of_range(
                std::format("Invalid half-hour offset: {}", numOfSeconds)
            );
        }

        const std::size_t startMinutes = MARKET_OPEN_MINUTES + (bucket * 30);
        const std::size_t endMinutes = startMinutes + 30;

        const std::size_t startHour = startMinutes / 60;
        const std::size_t startMinute = startMinutes % 60;

        const std::size_t endHour = endMinutes / 60;
        const std::size_t endMinute = endMinutes % 60;

        return std::format(
            "{:02}{:02}-{:02}{:02}",
            startHour,
            startMinute,
            endHour,
            endMinute
        );
    }

    std::filesystem::path HalfHourIndex::GetIndexPath(const std::size_t numOfSeconds) {
        return Common::Storage::INDEXES_BASE_PATH /
            std::format(
                "{}.index",
                GetHalfHourName(numOfSeconds)
            );
    }

    std::filesystem::path HalfHourIndex::GetMetadataPath(const std::size_t numOfSeconds) {
        return Common::Storage::METADATA_BASE_PATH /
            std::format(
                "{}.meta",
                GetHalfHourName(numOfSeconds)
            );
    }

    std::filesystem::path HalfHourIndex::GetRecordFilePathFromWindowLocation(const std::array<char,8> &yyyymmdd) {
        const std::string date(yyyymmdd.data(), yyyymmdd.size());
        const std::string formattedDate = 
            date.substr(0, 4) + "-" + 
            date.substr(4, 2) + "-" + 
            date.substr(6, 2);
        
        return Common::Storage::RECORDS_BASE_PATH / (formattedDate + ".rec");
    }

    std::array<char,8> HalfHourIndex::ToShortLocationArray(const std::filesystem::path &recordFilePath) {
        const std::string date = recordFilePath.stem().string();
        std::array<char, 8> shortDate{};
        std::size_t outputIndex = 0;

        for (const char c : date) {
            if (c != '-') {
                shortDate[outputIndex++] = c;
            }
        }

        return shortDate;
    }

    HalfHourIndex::HalfHourIndex(const std::size_t &numOfSeconds) : _index(DIMENSION) {
        this->_indexPath = GetIndexPath(numOfSeconds);
        this->_metadataPath = GetMetadataPath(numOfSeconds);

        if (Common::Storage::FileExists(this->_indexPath)) {
            Logger::Info(
                std::format(
                    "Loading FAISS index: {}",
                    this->_indexPath.string()
                ),
                true
            );

            // Using faiss api to load the (.)index file from path
            faiss::Index *loadedIndex = faiss::read_index(this->_indexPath.string().c_str());

            if (loadedIndex == nullptr) {
                throw std::runtime_error(
                    std::format(
                        "FAISS returned a null index for: {}",
                        this->_indexPath.string()
                    )
                );
            }

            faiss::IndexFlatL2 *flatIndex = dynamic_cast<faiss::IndexFlatL2 *>(loadedIndex);
            if (flatIndex == nullptr) {
                delete loadedIndex;

                throw std::runtime_error(
                    std::format(
                        "Index at {} is not an IndexFlatL2",
                        this->_indexPath.string()
                    )
                );
            }

            if (flatIndex->d != DIMENSION) {
                const int actualDimension = flatIndex->d;

                delete loadedIndex;

                throw std::runtime_error(
                    std::format(
                        "Index at {} has dimension {}, expected {}",
                        this->_indexPath.string(),
                        actualDimension,
                        DIMENSION
                    )
                );
            }

            this->_index = *flatIndex;
            delete loadedIndex;
            this->Updated = false;

            Logger::Info(
                std::format(
                    "Loaded {} vectors from {}",
                    this->_index.ntotal,
                    this->_indexPath.string()
                ),
                true
            );

            // Load metadata from file
            if (Common::Storage::FileExists(this->_metadataPath)) {
                LoadMetadata();
            }
        }
        else {
            Logger::Info(
                std::format(
                    "Creating new FAISS index: {}",
                    this->_indexPath.string()
                ),
                true
            );

            this->Updated = true;
        }
    }

    HalfHourIndex::~HalfHourIndex() {
        if (this->Updated) {
            try {
                Logger::Info(
                    std::format(
                        "Saving FAISS index: {}",
                        this->_indexPath.string()
                    ),
                    true
                );

                if (Common::Storage::FileExists(this->_indexPath.root_path()) == false) {
                    Common::Storage::CreateDirectory(this->_indexPath.root_path());
                }

                faiss::write_index(
                    &this->_index,
                    this->_indexPath.string().c_str()
                );

                SaveMetadataToDisk();
            }
            catch (const std::exception& e) {
                Logger::Error(
                    std::format(
                        "Failed to save HalfHourIndex: {}",
                        e.what()
                    ),
                    true
                );
            }
        }
    }

    void HalfHourIndex::LoadMetadata() {
        Logger::Info(
            std::format("Loading metadata: {}", this->_metadataPath.string()),
            true
        );

        this->_windowLocations.reserve(SECONDS_PER_HALF_HOUR);

        std::ifstream metaFile(this->_metadataPath, std::ios::binary);

        if (metaFile && metaFile.is_open()) {
            WindowLocation loc{};
            while (metaFile.read(reinterpret_cast<char*>(&loc), sizeof(loc))) {
                this->_windowLocations.push_back(loc);
            }
            if (metaFile.eof()) {
                Logger::Info("Successfully loaded metadata", true);
            }

            metaFile.close();
        }
        else {
            Logger::Error("Failed to load metadata file", true);
        }
    }

    void HalfHourIndex::SaveMetadataToDisk() {
        Logger::Info(
            std::format("Saving metadata: {}", this->_metadataPath.string()),
            true
        );

        if (Common::Storage::FileExists(this->_metadataPath.root_path()) == false) {
            Common::Storage::CreateDirectory(this->_metadataPath.root_path());
        }

        if (Common::Storage::FileExists(this->_metadataPath)) {
            Common::Storage::DeleteFile(this->_metadataPath);
        }

        std::ofstream metaFile(this->_metadataPath, std::ios::binary);

        if (metaFile && metaFile.is_open()) {
            for (const WindowLocation &loc : this->_windowLocations) {
                metaFile.write(reinterpret_cast<const char*>(&loc), sizeof(WindowLocation));

                if (!metaFile) {
                    Logger::Error(
                        std::format(
                            "Failed while writing metadata: {}",
                            this->_metadataPath.string()
                        ),
                        true
                    );
                    metaFile.close();
                    break;
                }
            }

            metaFile.close();

            Logger::Info(
                std::format(
                    "Successfully saved {} metadata entries",
                    this->_windowLocations.size()
                ),
                true
            );
        }
        else {
            Logger::Error(
                std::format(
                    "Failed to open metadata file for writing: {}",
                    this->_metadataPath.string()
                ),
                true
            );
        }
    }

    void HalfHourIndex::AddVectorsToIndex(
        const std::span<Core::MLRecord> &normalizedMLSpan,
        const std::filesystem::path &recFilePath,
        const std::size_t recordOffset
    ) {
        // A complete search window contins 30 MLRecords.
        constexpr std::size_t SEARCH_WINDOW_LENGTH = DIMENSION / 3;

        if (normalizedMLSpan.size() >= SEARCH_WINDOW_LENGTH) {
            // Number of complete 30-second windows that can be created.
            const std::size_t numVectors = normalizedMLSpan.size() - SEARCH_WINDOW_LENGTH + 1;

            Logger::Info(
                std::format(
                    "Adding {} vectors from {} at record offset {}",
                    numVectors,
                    recFilePath.string(),
                    recordOffset
                ),
                true
            );

            // Each vector contains DIMENSION (90) floats.
            std::vector<float> vectors{};
            vectors.reserve(numVectors * DIMENSION);

            const std::array<char, 8> tradingDay = ToShortLocationArray(recFilePath);

            for (std::size_t offset = 0; offset < numVectors; ++offset) {
                for (std::size_t windowIndex = 0; windowIndex < SEARCH_WINDOW_LENGTH; ++windowIndex) {
                    const MLRecord &record = normalizedMLSpan[offset + windowIndex];

                    vectors.push_back(record.mid_delta);
                    vectors.push_back(record.spread_delta);
                    vectors.push_back(record.volume);
                }

                WindowLocation location{};
                location.recordIndex = static_cast<uint32_t>(recordOffset + offset);
                location.tradingDay = tradingDay;

                this->_windowLocations.push_back(location);
            }

            // Add all generated vectors to FAISS in one operation.
            this->_index.add(
                static_cast<faiss::idx_t>(numVectors),
                vectors.data()
            );

            this->Updated = true;

            Logger::Info(
                std::format(
                    "Added {} vectors; index now contains {} vectors",
                    numVectors,
                    this->_index.ntotal
                ),
                true
            );
        }
        else {
            Logger::Warning(
                std::format(
                    "Not enough records to create a vector: {} records provided",
                    normalizedMLSpan.size()
                ),
                true
            );
        }
    }

    std::vector<HalfHourIndex::QueryResult> HalfHourIndex::Query(const std::vector<float> &normalizedQuery, const std::size_t maxResults) const {
        std::vector<QueryResult> res{};

        if (normalizedQuery.size() == DIMENSION) {
            if (maxResults > 0 && this->_index.ntotal > 0) {
                const faiss::idx_t resultCount = static_cast<faiss::idx_t>(
                    std::min(maxResults, static_cast<std::size_t>(this->_index.ntotal))
                );

                std::vector<float> distances(static_cast<std::size_t>(resultCount));
                std::vector<faiss::idx_t> ids(static_cast<std::size_t>(resultCount));

                this->_index.search(
                    1,
                    normalizedQuery.data(),
                    resultCount,
                    distances.data(),
                    ids.data()
                );

                res.reserve(static_cast<std::size_t>(resultCount));

                for (std::size_t i = 0; i < static_cast<std::size_t>(resultCount); ++i) {
                    const faiss::idx_t id = ids[i];
                    
                    if (id < 0 || static_cast<std::size_t>(id) >= this->_windowLocations.size()) {
                        Logger::Error(
                            std::format("Missing metadata for FAISS vector ID {}", id)
                        );
                        throw std::runtime_error(
                            std::format("Missing metadata for FAISS vector ID {}", id)
                        );
                    }

                    const WindowLocation &location = this->_windowLocations[static_cast<std::size_t>(id)];
                    res.push_back(QueryResult{
                        .distance = distances[i],
                        .tradingDayId = location.tradingDay,
                        .recordIndex = location.recordIndex
                    });
                }
            }
        }
        else {
            Logger::Error(
                std::format(
                    "Query dimension is {}, expected {}",
                    normalizedQuery.size(),
                    DIMENSION
                )
            );
            throw std::invalid_argument(
                std::format(
                    "Query dimension is {}, expected {}",
                    normalizedQuery.size(),
                    DIMENSION
                )
            );
        }

        return res;
    }

}