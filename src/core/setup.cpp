// setup.cpp
// Implementation for namespace holding references to setup processes.

#include "setup.hpp"
#include "../common/storage.hpp"
#include "../dukascopy/dukascopy.hpp"
#include "../common/logger.hpp"
#include "engine.hpp"
#include "transformer.hpp"
#include <cstddef>
#include <format>
#include <fstream>
#include <span>

namespace Core {
    namespace Setup {

        /**
         * @brief Helper to delete the corpus and records directories.
         * 
         */
        void DeleteAllStorageDirectories() {
            Logger::Info("Deleting all records..", true);
            Common::Storage::DeleteDirectory("storage/corpus");
            Common::Storage::DeleteDirectory("storage/records");
            Common::Storage::DeleteDirectory("storage/indexes");
        }

        void ExecuteTotalSetupProcess() {
            DeleteAllStorageDirectories();
            ExecuteDownloadAllRecordFiles();
            ExecuteUpdateIndexes();
        }

        void ExecuteDownloadAllRecordFiles() {
            Dukascopy::BeginCorpusExport(true);
        }

        void ExecuteDownloadAllMissingRecordFiles() {
            Dukascopy::BeginCorpusExport();
        }

        std::filesystem::path GetRecordPathForDay(const Common::DateTime &date) {
            return Common::Storage::RECORDS_BASE_PATH / (date.ToString_Date() + ".rec");
        }

        void ExecuteUpdateIndexes() {
            // For every .rec file under ~/storage/records/
            //      Grab the full day's MLRecords into a vector
            //      Pass the vector through normalization
            //      Split the vector into half-hour segments -> vector<vector<MLRecord>>
            //      For each half-hour segment
            //          Initialize HalfHourIndex object for the appropriate segment
            //          Pass segment vector<MLRecord> to HalfHourIndex.Add() method
            static_assert(sizeof(MLRecord) == 12);

            Logger::Info("========================================", true);
            Logger::Info("            Creating Indexes", true);
            Logger::Info("========================================", true);

            if (Common::Storage::FileExists(Common::Storage::RECORDS_BASE_PATH)) {
                for (Common::DateTime procDate = BEGIN_DATE; procDate <= END_DATE; procDate.NextDay()) {
                    std::filesystem::path recFilePath = GetRecordPathForDay(procDate);

                    if (Common::Storage::FileExists(recFilePath)) {
                        // Open and load into vector
                        std::vector<MLRecord> allRecords{};
                        allRecords.reserve(23000);
                        std::ifstream recFile(recFilePath, std::ios::binary);

                        if (recFile && recFile.is_open()) {
                            Logger::Info(std::format("Beginning read for: {}", recFilePath.string()), true);

                            MLRecord recordFromDisk{};
                            while (recFile.read(reinterpret_cast<char*>(&recordFromDisk), sizeof(recordFromDisk))) {
                                allRecords.push_back(recordFromDisk);
                            }
                            if (recFile.eof()) {
                                Logger::Info("Successfully parsed record file", true);
                            }
                            recFile.close();
                        }
                        else {
                            Logger::Error(std::format("Skipped: Could not open record file: {}", recFilePath.string()), true);
                            continue;
                        }

                        // Run the full day through a normalization process
                        NormalizeRecordsVector(allRecords);

                        // allRecords now contains MLRecords, where each item represents 1s
                        // Process in 30 minute segments (1800 items)
                        constexpr std::size_t RECORDS_PER_HALF_HOUR = 1800;

                        for (std::size_t i = 0; i < allRecords.size(); i += RECORDS_PER_HALF_HOUR) {
                            const std::size_t end = std::min(
                                i + RECORDS_PER_HALF_HOUR,
                                allRecords.size()
                            );

                            std::span<MLRecord> segment(allRecords.begin() + i, allRecords.begin() + end);
                            // The constructor is in charge of loading in the right index files based on the num of minutes since market start
                            HalfHourIndex indexSegment(i);
                            indexSegment.AddVectorsToIndex(segment);
                        }
                    }
                }
            }
        }

    }
}