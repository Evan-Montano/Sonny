// historicalrecordstore.cpp
// Implementation of the historical .rec file cache.

#include "historicalrecordstore.hpp"
#include "common/logger.hpp"
#include "common/storage.hpp"

#include <filesystem>
#include <format>
#include <fstream>

namespace Runtime {

    // ========== HISTORICAL RECORD STORE ========== //

    const std::vector<Core::MLRecord>* HistoricalRecordStore::Find(const std::array<char, 8>& tradingDayId) {
        // Index metadata stores "yyyymmdd"; record files are named "yyyy-mm-dd.rec".
        std::string date(tradingDayId.data(), tradingDayId.size());
        date.insert(4, "-");
        date.insert(7, "-");

        if (const auto cached = cache_.find(date); cached != cache_.end()) {
            return &cached->second;
        }

        if (cache_.size() >= MAX_CACHED_DAYS) {
            cache_.clear();
        }

        const std::filesystem::path recordPath = Common::Storage::RECORDS_BASE_PATH / (date + ".rec");
        std::ifstream file(recordPath, std::ios::binary | std::ios::ate);

        if (file.is_open() == false) {
            Logger::Warning(std::format("Unable to open historical record file: {}", recordPath.string()));
            return nullptr;
        }

        const std::streamoff fileSize = file.tellg();
        const std::streamoff recordSize = static_cast<std::streamoff>(sizeof(Core::MLRecord));

        if (fileSize < 0 || fileSize % recordSize != 0) {
            Logger::Warning(std::format("Invalid historical record file size: {}", recordPath.string()));
            return nullptr;
        }

        std::vector<Core::MLRecord> records(static_cast<std::size_t>(fileSize / recordSize));

        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(records.data()), static_cast<std::streamsize>(fileSize));

        if (file.gcount() != static_cast<std::streamsize>(fileSize)) {
            Logger::Warning(std::format("Failed reading historical record file: {}", recordPath.string()));
            return nullptr;
        }

        return &cache_.emplace(date, std::move(records)).first->second;
    }

    // ========== END HISTORICAL RECORD STORE ========== //

}
