#include "dukascopy/failedrecordfiles.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {
    /**
     * @brief Isolates failed-download tracking tests in a temporary records directory.
     */
    class FailedRecordFilesTests : public ::testing::Test {
    protected:
        // MEMBERS

        std::filesystem::path Directory;
        std::filesystem::path TrackingPath;

        // METHODS

        void SetUp() override {
            Directory = std::filesystem::temp_directory_path() /
                ("sonny_failed_records_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(Directory);
            TrackingPath = Directory / "records/failed_downloads.json";
        }

        void TearDown() override { std::filesystem::remove_all(Directory); }
    };
}

TEST_F(FailedRecordFilesTests, MissingTrackingFileDoesNotSkipOrWriteAnything) {
    Dukascopy::FailedRecordFiles failures(TrackingPath);
    EXPECT_FALSE(failures.ShouldSkip("2026-11-05.rec", false));
    failures.ClearFailure("2026-11-05.rec");
    EXPECT_FALSE(std::filesystem::exists(TrackingPath));
}

TEST_F(FailedRecordFilesTests, FailedDaysPersistAcrossRunsAndFullDownloadsRetryThem) {
    {
        Dukascopy::FailedRecordFiles failures(TrackingPath);
        failures.MarkFailed("2026-11-05.rec");
        // Each failure is already on disk, without waiting for the end of the run or destruction.
        const Dukascopy::FailedRecordFiles reloaded(TrackingPath);
        EXPECT_TRUE(reloaded.ShouldSkip("2026-11-05.rec", false));
        EXPECT_FALSE(reloaded.ShouldSkip("2026-11-05.rec", true));
        EXPECT_FALSE(reloaded.ShouldSkip("2026-11-06.rec", false));
    }
    const Dukascopy::FailedRecordFiles nextRun(TrackingPath);
    EXPECT_TRUE(nextRun.ShouldSkip("2026-11-05.rec", false));
}

TEST_F(FailedRecordFilesTests, SuccessfulRetriesRemoveOnlyTheRecoveredDay) {
    Dukascopy::FailedRecordFiles failures(TrackingPath);
    failures.MarkFailed("2026-11-05.rec");
    failures.MarkFailed("2026-11-06.rec");
    failures.ClearFailure("2026-11-05.rec");

    const Dukascopy::FailedRecordFiles nextRun(TrackingPath);
    EXPECT_FALSE(nextRun.ShouldSkip("2026-11-05.rec", false));
    EXPECT_TRUE(nextRun.ShouldSkip("2026-11-06.rec", false));
    failures.ClearFailure("2026-11-06.rec");
    EXPECT_TRUE(Common::JsonUtility::FromFile(TrackingPath).Get<std::vector<std::string>>("failed_record_files").empty());
}

TEST_F(FailedRecordFilesTests, UnchangedResultsDoNotRewriteTheFile) {
    Dukascopy::FailedRecordFiles failures(TrackingPath);
    failures.MarkFailed("2026-11-05.rec");
    // Make the destination unwritable as a file, even for root. Unchanged results need no save.
    std::filesystem::remove(TrackingPath);
    std::filesystem::create_directories(TrackingPath / "blocker");
    EXPECT_NO_THROW(failures.MarkFailed("2026-11-05.rec"));
    EXPECT_NO_THROW(failures.ClearFailure("unknown.rec"));
    EXPECT_TRUE(std::filesystem::is_directory(TrackingPath));
}

TEST_F(FailedRecordFilesTests, SavingFailuresDoesNotChangeMemoryUntilPersistenceSucceeds) {
    Dukascopy::FailedRecordFiles failures(TrackingPath);
    std::filesystem::create_directories(TrackingPath / "blocker");
    EXPECT_THROW(failures.MarkFailed("2026-11-05.rec"), std::filesystem::filesystem_error);
    EXPECT_FALSE(failures.ShouldSkip("2026-11-05.rec", false));

    std::filesystem::remove_all(TrackingPath);
    failures.MarkFailed("2026-11-05.rec");
    const auto backup = Directory / "saved.json";
    std::filesystem::rename(TrackingPath, backup);
    std::filesystem::create_directories(TrackingPath / "blocker");
    EXPECT_THROW(failures.ClearFailure("2026-11-05.rec"), std::filesystem::filesystem_error);
    EXPECT_TRUE(failures.ShouldSkip("2026-11-05.rec", false));
    EXPECT_TRUE(Dukascopy::FailedRecordFiles(backup).ShouldSkip("2026-11-05.rec", false));
}

TEST_F(FailedRecordFilesTests, DuplicateEntriesAreDeduplicatedAndMalformedFilesAreRejected) {
    Common::Storage::CreateDirectory(TrackingPath.parent_path());
    Common::Storage::WriteFile(TrackingPath, R"({"failed_record_files":["2026-11-05.rec","2026-11-05.rec"]})");
    Dukascopy::FailedRecordFiles failures(TrackingPath);
    failures.MarkFailed("2026-11-06.rec");
    const auto files = Common::JsonUtility::FromFile(TrackingPath).Get<std::vector<std::string>>("failed_record_files");
    EXPECT_EQ(files, (std::vector<std::string>{"2026-11-05.rec", "2026-11-06.rec"}));

    Common::Storage::WriteFile(TrackingPath, "invalid json");
    EXPECT_THROW((Dukascopy::FailedRecordFiles{TrackingPath}), std::invalid_argument);
    EXPECT_EQ(Common::Storage::ReadFile(TrackingPath), "invalid json");
}
