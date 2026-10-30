// setup.cpp
// Implementation for namespace holding references to setup processes.

#include "setup.hpp"
#include "../common/storage.hpp"
#include "../dukascopy/dukascopy.hpp"
#include "../common/logger.hpp"
#include "engine.hpp"
#include "transformer.hpp"
#include "../ui/ui.hpp"

#include <cstddef>
#include <format>
#include <fstream>
#include <ftxui/dom/elements.hpp>
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
        
        void ExecuteUpdateIndexes() {
            using namespace Common::Storage;

            constexpr std::size_t RECORDS_PER_HALF_HOUR = 1800;
            constexpr std::size_t SEARCH_WINDOW_LENGTH = 30;
            constexpr std::size_t TOTAL_SEGMENTS = 13;

            const std::size_t totalDays = static_cast<std::size_t>(Core::Setup::TOTAL_DAYS);
            const std::size_t totalUnits = totalDays * TOTAL_SEGMENTS;

            std::string status{};
            std::string currentDate{};
            std::string currentSegment{};
            std::size_t completedUnits = 0;

            auto view = UI::Get().PushView([&] {
                const float progress =
                    totalUnits == 0
                        ? 0.0f
                        : static_cast<float>(completedUnits) /
                            static_cast<float>(totalUnits);

                return ftxui::window(
                    ftxui::text("Updating FAISS Indexes") | ftxui::bold,
                    ftxui::vbox({
                        ftxui::text(status),
                        ftxui::separator(),
                        ftxui::hbox({
                            ftxui::text("Current segment: "),
                            ftxui::text(currentSegment) | ftxui::bold,
                        }),
                        ftxui::hbox({
                            ftxui::text("Current date: "),
                            ftxui::text(currentDate) | ftxui::bold,
                        }),
                        ftxui::gauge(progress),
                        ftxui::text(
                            std::format(
                                "{} / {} segment-days",
                                completedUnits,
                                totalUnits
                            )
                        )
                    }) |
                    ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 55)
                );
            });

            UI::Get().Refresh();

            if (FileExists(RECORDS_BASE_PATH)) {
                // Rebuild from scratch. Loading the existing index would add every day's vectors a second time.
                Logger::Info("Deleting existing indexes and metadata..", true);
                DeleteDirectory(INDEXES_BASE_PATH);
                DeleteDirectory(METADATA_BASE_PATH);

                // Process one half-hour index at a time.
                for (std::size_t segmentIndex = 0; segmentIndex < TOTAL_SEGMENTS; ++segmentIndex) {
                    // The constructor uses the record offset to select the index.
                    const std::size_t segmentOffset = segmentIndex * RECORDS_PER_HALF_HOUR;
                    UI::Get().Update([&] {
                        currentSegment = std::format("{} / {}", segmentIndex + 1, TOTAL_SEGMENTS);
                    });

                    // Keep this index alive across the entire historical date range.
                    // Its destructor saves the index and metadata when this scope ends.
                    {
                        Core::HalfHourIndex indexSegment(segmentOffset);

                        Common::DateTime dt = Core::Setup::BEGIN_DATE;

                        for (; dt <= Core::Setup::END_DATE; dt.NextDay()) {
                            const std::string date = dt.ToString_Date();
                            const std::filesystem::path recFilePath = RECORDS_BASE_PATH / (date + ".rec");

                            UI::Get().Update([&] {
                                currentDate = date;
                                status = std::format("Indexing {}", recFilePath.filename().string());
                            });

                            // Mising record files are skipped, as in the existing flow.
                            if (FileExists(recFilePath)) {
                                std::ifstream recFileStream(recFilePath, std::ios::binary);

                                if (recFileStream.is_open()) {
                                    // Load only one day's records into memory at a time.
                                    std::vector<Core::MLRecord> allRecords{};
                                    allRecords.reserve(23000);

                                    Core::MLRecord record{};

                                    while (recFileStream.read(reinterpret_cast<char*>(&record), sizeof(record))) {
                                        allRecords.push_back(record);
                                    }

                                    recFileStream.close();

                                    // Normalize the complete trading day before extracting a segment.
                                    // Normalizing segments independently would change the existing vector values.
                                    NormalizeRecordsVector(allRecords);

                                    // Only process this segment if the day has records
                                    // beginning at its offset.
                                    if (allRecords.size() > segmentOffset) {
                                        const std::size_t end = std::min(
                                            segmentOffset + RECORDS_PER_HALF_HOUR + (SEARCH_WINDOW_LENGTH - 1), 
                                            allRecords.size()
                                        );

                                        // Include 29 additional records so the final window in this segment
                                        // can contain 30 records.
                                        std::span<Core::MLRecord> segment(
                                            allRecords.begin() + segmentOffset,
                                            allRecords.begin() + end
                                        );

                                        indexSegment.AddVectorsToIndex(segment, recFilePath, segmentOffset);
                                    }

                                    // allRecordsd is released at the end of this scope
                                    // before the net historical day is loaded.
                                }
                            }

                            UI::Get().Update([&] { ++completedUnits; });
                        }
                    }

                    // indexSegment is destroyed here, saving the updated index and
                    // metadata once after every date has been processed for this segment.
                    UI::Get().Refresh();
                }
            }
            else {
                Logger::Error("Records directory does not exist.", true);
            }

            UI::Get().Update([&] {
                status = "Finished updating FAISS indexes";
                currentDate.clear();
                currentSegment.clear();
            });
        }

    }
}