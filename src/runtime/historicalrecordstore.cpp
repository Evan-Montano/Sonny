// historicalrecordstore.cpp
// Implementation of the historical .rec file cache.

#include "historicalrecordstore.hpp"
#include "common/logger.hpp"
#include "common/storage.hpp"

#include <filesystem>
#include <format>
#include <exception>

namespace Runtime {

    // ========== HISTORICAL RECORD STORE ========== //

    const std::vector<Core::MLRecord>* HistoricalRecordStore::Find(const std::array<char, 8>& tradingDayId) {
        // Index metadata stores "yyyymmdd"; record files are named "yyyy-mm-dd.rec".
        std::string date(tradingDayId.data(), tradingDayId.size());
        date.insert(4, "-");
        date.insert(7, "-");

        if (const auto cached = Cache.find(date); cached != Cache.end()) {
            return &cached->second;
        }

        if (Cache.size() >= MAX_CACHED_DAYS) {
            Cache.clear();
        }

        const std::filesystem::path recordPath = Common::Storage::RECORDS_BASE_PATH / (date + ".rec");
        try {
            auto records = Common::Storage::ReadBinaryRecords<Core::MLRecord>(recordPath);
            return &Cache.emplace(date, std::move(records)).first->second;
        }
        catch (const std::exception& error) {
            Logger::Warning(std::format("Unable to load historical record file {}: {}", recordPath.string(), error.what()));
            return nullptr;
        }
    }

    // ========== END HISTORICAL RECORD STORE ========== //

}
