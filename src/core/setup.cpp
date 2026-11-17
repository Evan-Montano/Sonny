// setup.cpp
// Implementation for namespace holding references to setup processes.

#include "setup.hpp"
#include "../common/storage.hpp"
#include "../dukascopy/dukascopy.hpp"
#include "../common/logger.hpp"
#include "core/structures.hpp"
#include "halfhourmultiindex.hpp"
#include "transformer.hpp"
#include "inputconfig.hpp"
#include "weekdayconfig.hpp"
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
            constexpr std::size_t TOTAL_SEGMENTS = 13;

            const std::size_t totalDays = static_cast<std::size_t>(Core::Setup::TOTAL_DAYS);

            // The runtime's active windows are the only ones built, so every index the runtime
            // looks for is one this build was asked to create.
            const std::vector<std::size_t> inputWindows = Core::ActiveInputWindows();

            // Two passes for every segment-day:
            // 1. Calculate normalization statistics.
            // 2. Normalize records and build the index.
            // Passes that are not needed (reused indexes) count as done straight away.
            const std::size_t totalUnits = totalDays * TOTAL_SEGMENTS * 2 * inputWindows.size();

            std::string status{};
            std::string currentDate{};
            std::string currentSegment{};
            std::string currentWindow{};

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
                            ftxui::text("Input window: "),
                            ftxui::text(currentWindow) | ftxui::bold,
                        }),

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
                                "{} / {} processing units",
                                completedUnits,
                                totalUnits
                            )
                        )
                    }) |
                    ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 55)
                );
            });

            UI::Get().Refresh();

            // Helper for loading a complete raw .rec file.
            auto LoadRecords = [](const std::filesystem::path& path) {
                std::vector<Core::MLRecord> records{};
                records.reserve(23000);

                std::ifstream stream(path, std::ios::binary);

                if (stream.is_open() == false) {
                    throw std::runtime_error(
                        "Unable to open record file: " + path.string()
                    );
                }

                Core::MLRecord record{};

                while (stream.read(
                    reinterpret_cast<char*>(&record),
                    sizeof(record)
                )) {
                    records.push_back(record);
                }

                // Detect incomplete records or actual I/O failures.
                if (stream.bad() || stream.gcount() != 0 || stream.eof() == false) {
                    throw std::runtime_error(
                        "Failed reading record file: " + path.string()
                    );
                }

                stream.close();

                return records;
            };

            if (FileExists(RECORDS_BASE_PATH)) {

                Logger::Info(
                    std::format(
                        "Updating indexes for {} input window(s); direction search mode {}; {} existing complete indexes.",
                        inputWindows.size(),
                        Core::DirectionSearchModeName(Core::DIRECTION_SEARCH_MODE),
                        Core::Setup::REUSE_EXISTING_INDEXES ? "reusing" : "rebuilding"
                    ),
                    true
                );

                // Build independent normalization parameters and FAISS vectors for each window.
                for (const std::size_t inputWindowSeconds : inputWindows) {
                    const std::size_t searchWindowPadding = inputWindowSeconds - 1;

                    UI::Get().Update([&] {
                        currentWindow = std::format("{}s", inputWindowSeconds);
                    });

                    // Process one half-hour index at a time.
                    for (std::size_t segmentIndex = 0; segmentIndex < TOTAL_SEGMENTS; ++segmentIndex) {
                        const std::size_t segmentOffset = segmentIndex * RECORDS_PER_HALF_HOUR;

                        UI::Get().Update([&] {
                            currentSegment = std::format(
                                "{} / {}",
                                segmentIndex + 1,
                                TOTAL_SEGMENTS
                            );
                        });

                        // Only the sub-indexes this segment still needs: magnitude, the all-week direction
                        // index, and/or one direction index per weekday group (see SubIndexSelection::ToBuild).
                        const HalfHourMultiIndex::SubIndexSelection subIndexes = HalfHourMultiIndex::SubIndexSelection::ToBuild(
                            segmentOffset,
                            inputWindowSeconds,
                            Core::DIRECTION_SEARCH_MODE,
                            Core::DIRECTION_WEEKDAY_GROUPS,
                            Core::Setup::REUSE_EXISTING_INDEXES
                        );

                        if (subIndexes.Empty()) {
                            Logger::Info(
                                std::format("{}s segment {}: every needed index already exists; reusing them.", inputWindowSeconds, segmentIndex + 1),
                                true
                            );

                            UI::Get().Update([&] {
                                completedUnits += totalDays * 2;
                            });

                            continue;
                        }

                        std::string weekdayGroupNames{};
                        for (const Core::WeekdayGroup &group : subIndexes.weekday_groups) {
                            weekdayGroupNames += std::format("{}{}", weekdayGroupNames.empty() ? "" : ", ", group.name);
                        }

                        Logger::Info(
                            std::format(
                                "{}s segment {}: building magnitude: {}, AllDays direction: {}, weekday direction: [{}].",
                                inputWindowSeconds,
                                segmentIndex + 1,
                                subIndexes.magnitude ? "yes" : "reused",
                                subIndexes.all_days_direction ? "yes" : (Core::SearchesAllDaysDirection(Core::DIRECTION_SEARCH_MODE) ? "reused" : "not needed"),
                                weekdayGroupNames
                            ),
                            true
                        );

                        // Rebuilt sub-indexes start empty; an existing file would otherwise be loaded and appended to.
                        HalfHourMultiIndex::DeleteFiles(segmentOffset, inputWindowSeconds, subIndexes);

                        // The index remains alive throughout both passes.
                        // Its destructor persists the index, metadata, normalization JSON, and weekday manifests.
                        {
                            HalfHourMultiIndex indexSegment(segmentOffset, inputWindowSeconds, subIndexes);

                            // Only the magnitude index uses normalization parameters.
                            if (subIndexes.magnitude) {

                                // ======================================== //
                                // PASS 1: CALCULATE NORMALIZATION PARAMETERS
                                // ======================================== //

                                Core::NormalizationAccumulator accumulator{};

                                Common::DateTime dt = Core::Setup::BEGIN_DATE;

                                for (; dt <= Core::Setup::END_DATE; dt.NextDay()) {
                                    const std::string date = dt.ToString_Date();

                                    const std::filesystem::path recFilePath = RECORDS_BASE_PATH / (date + ".rec");

                                    UI::Get().Update([&] {
                                        currentDate = date;

                                        status = std::format(
                                            "Calculating normalization statistics: {}",
                                            recFilePath.filename().string()
                                        );
                                    });

                                    if (FileExists(recFilePath)) {
                                        std::vector<Core::MLRecord> allRecords = LoadRecords(recFilePath);

                                        if (allRecords.size() > segmentOffset) {
                                            const std::size_t end = std::min(
                                                segmentOffset +
                                                    RECORDS_PER_HALF_HOUR +
                                                    searchWindowPadding,
                                                allRecords.size()
                                            );

                                            const std::size_t segmentSize = end - segmentOffset;

                                            // A segment must contain at least one complete configured window.
                                            if (segmentSize >= inputWindowSeconds) {

                                                // ========= IMPORTANT ======== //
                                                // Normalization for the magnitude sub-index requires the data to be in it's
                                                // absolute value, so we are creating a copy of the chosen segment, and
                                                // taking each position's absolute value before passing it to the accumulator.
                                                std::vector<Core::MLRecord> segment(
                                                    allRecords.begin() + segmentOffset,
                                                    allRecords.begin() + end
                                                );

                                                for (Core::MLRecord &rec : segment) {
                                                    rec.mid_delta = std::abs(rec.mid_delta);
                                                }

                                                accumulator.AddRecords(segment);
                                            }
                                        }
                                    }

                                    UI::Get().Update([&] {
                                        ++completedUnits;
                                    });
                                }

                                // Calculate the final parameters across all historical days.
                                const Core::NormalizationParameters parameters = accumulator.GetParameters();

                                Logger::Info(
                                    std::format(
                                        "Calculated normalization parameters for segment {} "
                                        "using {} historical records.",
                                        segmentIndex + 1,
                                        parameters.sample_count
                                    ),
                                    true
                                );

                                // Associate these parameters with this specific FAISS index.

                                indexSegment.SetNormalizationParameters(parameters);
                            }
                            else {
                                UI::Get().Update([&] {
                                    completedUnits += totalDays;
                                });
                            }

                            // ======================================== //
                            // PASS 2: NORMALIZE RECORDS AND BUILD INDEX
                            // ======================================== //

                            // ========= IMPORTANT ======== //
                            // Days are added NEWEST FIRST. FAISS breaks distance ties by vector ID (lowest
                            // wins), and IDs follow insertion order, so this makes tied matches resolve to the
                            // most recent days. The direction index relies on it: short windows have only a few
                            // thousand distinct {-1, 0, 1} patterns, so a query can tie with thousands of
                            // vectors and oldest-first insertion would always pick the oldest days.
                            Common::DateTime dt = Core::Setup::END_DATE;

                            for (; dt >= Core::Setup::BEGIN_DATE; dt.PreviousDay()) {
                                const std::string date = dt.ToString_Date();

                                const std::filesystem::path recFilePath = RECORDS_BASE_PATH / (date + ".rec");

                                UI::Get().Update([&] {
                                    currentDate = date;

                                    status = std::format(
                                        "Building FAISS vectors: {}",
                                        recFilePath.filename().string()
                                    );
                                });

                                if (FileExists(recFilePath)) {
                                    std::vector<Core::MLRecord> allRecords = LoadRecords(recFilePath);

                                    if (allRecords.size() > segmentOffset) {
                                        const std::size_t end = std::min(
                                            segmentOffset +
                                                RECORDS_PER_HALF_HOUR +
                                                searchWindowPadding,
                                            allRecords.size()
                                        );

                                        const std::size_t segmentSize = end - segmentOffset;

                                        if (segmentSize >= inputWindowSeconds) {
                                            std::span<Core::MLRecord> segment(
                                                allRecords.data() + segmentOffset,
                                                segmentSize
                                            );

                                            // Raw segment can go in as-is because the HalfHourMultiIndex handles
                                            // splitting and normalization. Each weekday-group index keeps only
                                            // this day if the day's New York weekday is in its group.
                                            indexSegment.AddVectorsToIndex(segment, recFilePath, segmentOffset);
                                        }
                                    }
                                }

                                UI::Get().Update([&] {
                                    ++completedUnits;
                                });
                            }
                        }

                        // indexSegment is destroyed here, saving all three files.
                        UI::Get().Refresh();
                    }
                }
            }
            else {
                Logger::Error("Records directory does not exist.", true);
            }

            UI::Get().Update([&] {
                status = "Finished updating FAISS indexes";
                currentDate.clear();
                currentSegment.clear();
                currentWindow.clear();
            });

            UI::Get().Refresh();
        }

    }
}
