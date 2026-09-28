// engine.cpp
// Class and method implementations for all things pertaining to the building and querying of market futures.

#include "engine.hpp"
#include "../common/storage.hpp"
#include "../common/logger.hpp"

#include <format>
#include <stdexcept>

namespace Core {

    std::string HalfHourIndex::GetHalfHourName(const std::size_t numOfMinutes) {
        const std::size_t bucket = numOfMinutes / SECONDS_PER_HALF_HOUR;
        
        if (bucket >= TOTAL_HALF_HOURS) {
            throw std::out_of_range(
                std::format("Invalid half-hour offset: {}", numOfMinutes)
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

    std::filesystem::path HalfHourIndex::GetIndexPath(const std::size_t numOfMinutes) {
        return Common::Storage::INDEXES_BASE_PATH /
            std::format(
                "{}.index",
                GetHalfHourName(numOfMinutes)
            );
    }

    std::filesystem::path HalfHourIndex::GetMetadataPath(const std::size_t numOfMinutes) {
        return Common::Storage::METADATA_BASE_PATH /
            std::format(
                "{}.meta",
                GetHalfHourName(numOfMinutes)
            );
    }

    HalfHourIndex::HalfHourIndex(const std::size_t &numOfMinutes) : _index(DIMENSION) {
        this->_indexPath = GetIndexPath(numOfMinutes);
        this->_metadataPath = GetMetadataPath(numOfMinutes);

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

            // TODO: Load metadata from SQL
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
            Logger::Info(
                std::format(
                    "Saving FAISS index: {}",
                    this->_indexPath.string()
                ),
                true
            );

            faiss::write_index(
                &this->_index,
                this->_indexPath.string().c_str()
            );
        }
    }

}