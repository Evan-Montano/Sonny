#include "common/storage.hpp"

#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iterator>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

namespace {
    namespace Storage = Common::Storage;

    class ScopedPermissions {
        std::filesystem::path path_;
        std::filesystem::perms original_;
    public:
        ScopedPermissions(const std::filesystem::path& path, std::filesystem::perms permissions)
            : path_(path), original_(std::filesystem::status(path).permissions()) {
            std::filesystem::permissions(path, permissions);
        }
        ~ScopedPermissions() {
            std::error_code ignored;
            std::filesystem::permissions(path_, original_, ignored);
        }
    };

    class StorageTests : public ::testing::Test {
    protected:
        std::filesystem::path previous_;
        std::filesystem::path directory_;

        void SetUp() override {
            previous_ = std::filesystem::current_path();
            directory_ = std::filesystem::temp_directory_path() /
                ("sonny_storage_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(directory_);
            std::filesystem::current_path(directory_);
        }
        void TearDown() override {
            std::filesystem::current_path(previous_);
            std::filesystem::remove_all(directory_);
        }
    };
}

TEST_F(StorageTests, CheckedContentIoPreservesBinaryBytesAndRejectsPartialRecords) {
    const std::string contents("text\0with\nbytes", 15);
    Storage::WriteFile("bytes", contents);
    EXPECT_EQ(Storage::ReadFile("bytes"), contents);
    std::error_code error;
    EXPECT_EQ(Storage::FileSize("bytes", error), contents.size());
    EXPECT_FALSE(error);

    const std::array<std::int32_t, 3> records{-1, 0, 1234};
    Storage::WriteBinaryRecords<std::int32_t>("records", records);
    EXPECT_EQ(Storage::ReadBinaryRecords<std::int32_t>("records"), (std::vector<std::int32_t>{-1, 0, 1234}));
    Storage::WriteFile("records", std::string(5, 'x'));
    EXPECT_THROW(Storage::ReadBinaryRecords<std::int32_t>("records"), std::runtime_error);
    EXPECT_THROW(Storage::ReadFile("missing"), std::runtime_error);
    EXPECT_THROW(Storage::WriteFile("missing-parent/file", "data"), std::runtime_error);
    Storage::WriteFile("empty", "");
    EXPECT_TRUE(Storage::ReadBinaryRecords<std::int32_t>("empty").empty());
    EXPECT_TRUE(Storage::ReadFile("empty").empty());
}

TEST_F(StorageTests, ErrorResultsDistinguishMissingFilesFromFailures) {
    std::error_code error = std::make_error_code(std::errc::permission_denied);
    EXPECT_FALSE(Storage::FileExists("missing", error));
    EXPECT_FALSE(error);
    EXPECT_FALSE(Storage::DeleteFile("missing", error));
    EXPECT_FALSE(error);
    EXPECT_TRUE(Storage::CreateDirectory("directory", error));
    EXPECT_FALSE(error);
    EXPECT_FALSE(Storage::CreateDirectory("directory", error));
    EXPECT_FALSE(error);

    Storage::WriteFile("file", "retained");
    EXPECT_FALSE(Storage::CreateDirectory("file/child", error));
    EXPECT_TRUE(error);
    Storage::RenameFile("missing", "file", error);
    EXPECT_TRUE(error);
    EXPECT_EQ(Storage::ReadFile("file"), "retained");
    Storage::WriteFile("new", "replacement");
    Storage::RenameFile("new", "file", error);
    EXPECT_FALSE(error);
    EXPECT_EQ(Storage::ReadFile("file"), "replacement");
    EXPECT_TRUE(Storage::DeleteFile("file", error));
    EXPECT_FALSE(error);
    Storage::FileSize("file", error);
    EXPECT_TRUE(error);
}

TEST_F(StorageTests, AtomicWritesCreateParentsAndReplaceCompleteContents) {
    Storage::WriteFileAtomically("nested/file", "first");
    Storage::WriteFileAtomically("nested/file", "replacement");
    EXPECT_EQ(Storage::ReadFile("nested/file"), "replacement");
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator("nested")) {
        EXPECT_EQ(entry.path().filename(), "file");
        ++entries;
    }
    EXPECT_EQ(entries, 1u);
}

TEST_F(StorageTests, FailedAtomicReplacementPreservesDestinationAndCleansStaging) {
    std::filesystem::create_directory("destination");
    Storage::WriteFile("destination/retained", "old contents");
    EXPECT_THROW(Storage::WriteFileAtomically("destination", "new contents"), std::filesystem::filesystem_error);
    EXPECT_EQ(Storage::ReadFile("destination/retained"), "old contents");
    for (const auto& entry : std::filesystem::directory_iterator(".")) {
        EXPECT_EQ(entry.path().filename(), "destination");
    }
}

TEST_F(StorageTests, ConcurrentAtomicWritersNeverExposeMixedContents) {
    const std::string first(8192, 'a');
    const std::string second(8192, 'b');
    Storage::WriteFileAtomically("shared", first);
    std::atomic<bool> failed{false};
    auto writer = [&](const std::string& contents) {
        try {
            for (int i = 0; i < 20; ++i) Storage::WriteFileAtomically("shared", contents);
        } catch (...) { failed = true; }
    };
    std::jthread left(writer, std::cref(first));
    std::jthread right(writer, std::cref(second));
    for (int i = 0; i < 40; ++i) {
        const auto contents = Storage::ReadFile("shared");
        EXPECT_TRUE(contents == first || contents == second);
    }
    left.join();
    right.join();
    EXPECT_FALSE(failed);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator("."), std::filesystem::directory_iterator{}), 1);
}

TEST_F(StorageTests, SettingsCanInitializeInAnExistingDirectoryAndSaveRepeatedly) {
    std::filesystem::create_directories("storage/settings");
    Common::Storage::Settings* settings = nullptr;
    ASSERT_NO_THROW(settings = &Common::Storage::Settings::Get());
    ASSERT_NO_THROW(settings->Reload());
    settings->Reset();
    settings->Set("input", 20);
    settings->Set("nullable", nullptr);
    ASSERT_NO_THROW(settings->Save());
    settings->Set("input", 45);
    ASSERT_NO_THROW(settings->Save());
    settings->Set("input", 30);
    ASSERT_NO_THROW(settings->Reload());
    EXPECT_EQ(settings->Get<int>("input"), 45);
    EXPECT_TRUE(settings->Has("nullable"));
    EXPECT_FALSE(settings->Has("missing"));
    EXPECT_TRUE(settings->Remove("nullable"));
    EXPECT_FALSE(settings->Has("nullable"));
    const auto persisted = Common::JsonUtility::FromFile("storage/settings/sonny_settings.json");
    EXPECT_EQ(persisted.Get<int>("input"), 45);
    EXPECT_TRUE(persisted.Contains("START_DATE_MS"));
    EXPECT_TRUE(persisted.Contains("END_DATE_MS"));
}

TEST_F(StorageTests, AtomicReplacementRetainsPermissionBits) {
    const auto shared = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                        std::filesystem::perms::group_read;
    Storage::WriteFile("shared", "old");
    std::filesystem::permissions("shared", shared);
    Storage::WriteFileAtomically("shared", "new");
    EXPECT_EQ(Storage::ReadFile("shared"), "new");
    EXPECT_EQ(std::filesystem::status("shared").permissions() & std::filesystem::perms::mask, shared);
    Storage::WriteFileAtomically("private", "new");
    EXPECT_EQ(std::filesystem::status("private").permissions() & std::filesystem::perms::mask,
              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

TEST_F(StorageTests, AtomicWritesSupportLongDestinationNames) {
    const std::string name(250, 'x');
    Storage::WriteFile(name, "original");
    ASSERT_NO_THROW(Storage::WriteFileAtomically(name, "replacement"));
    EXPECT_EQ(Storage::ReadFile(name), "replacement");
    Storage::DeleteFile(name);
    ASSERT_NO_THROW(Storage::WriteFileAtomically(name, "created"));
    EXPECT_EQ(Storage::ReadFile(name), "created");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator("."), std::filesystem::directory_iterator{}), 1);
}

TEST_F(StorageTests, FailedStagingLeavesTheExistingFileIntact) {
    if (::geteuid() == 0) GTEST_SKIP() << "Root bypasses directory write restrictions.";
    Storage::CreateDirectory("locked");
    Storage::WriteFile("locked/file", "original");
    const ScopedPermissions locked("locked", std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    EXPECT_THROW(Storage::WriteFileAtomically("locked/file", "replacement"), std::system_error);
    EXPECT_EQ(Storage::ReadFile("locked/file"), "original");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator("locked"), std::filesystem::directory_iterator{}), 1);
}

TEST_F(StorageTests, AtomicStagingDescriptorsAreCloseOnExec) {
    // Keep staging active until inspected, rather than relying on a single write's timing.
    std::atomic<bool> failed{false};
    std::jthread writer([&](std::stop_token stop) {
        try {
            const std::string contents(1024 * 1024, 'x');
            while (!stop.stop_requested()) Storage::WriteFileAtomically("target", contents);
        } catch (...) { failed = true; }
    });
    bool observed = false;
    bool closeOnExec = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (!observed && !failed && std::chrono::steady_clock::now() < deadline) {
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
            std::error_code error;
            const auto target = std::filesystem::read_symlink(entry.path(), error);
            if (error || target.parent_path() != directory_) continue;
            const int flags = ::fcntl(std::stoi(entry.path().filename().string()), F_GETFD);
            if (flags < 0) continue; // The writer closed it while it was being inspected.
            observed = true;
            closeOnExec = (flags & FD_CLOEXEC) != 0;
            break;
        }
    }
    writer.request_stop();
    writer.join();
    EXPECT_FALSE(failed);
    ASSERT_TRUE(observed) << "No live staging descriptor was observed.";
    EXPECT_TRUE(closeOnExec);
}

TEST_F(StorageTests, SettingsRoundTripSignedMillisecondsWithoutPrecisionLoss) {
    auto& settings = Storage::Settings::Get();
    settings.Reset();
    settings.START_DATE = Common::DateTime::FromTimestampMilliseconds(1234, Common::TimeZone::UTC);
    settings.END_DATE = Common::DateTime::FromTimestampMilliseconds(-1, Common::TimeZone::UTC);
    const auto start = settings.START_DATE;
    const auto end = settings.END_DATE;
    settings.Save();
    settings.START_DATE.SetTimestamp(0);
    settings.END_DATE.SetTimestamp(0);
    settings.Reload();
    EXPECT_TRUE(settings.START_DATE.Equal(start));
    EXPECT_TRUE(settings.END_DATE.Equal(end));
    const auto json = Common::JsonUtility::FromFile("storage/settings/sonny_settings.json");
    EXPECT_EQ(json.Get<std::int64_t>("START_DATE_MS"), 1234);
    EXPECT_EQ(json.Get<std::int64_t>("END_DATE_MS"), -1);
}

TEST_F(StorageTests, LegacySecondsSettingsRemainReadableAndAreMigratedOnSave) {
    Storage::CreateDirectory("storage/settings");
    Storage::WriteFile("storage/settings/sonny_settings.json", R"({"START_DATE":1,"END_DATE":-1,"input":20})");
    auto& settings = Storage::Settings::Get();
    settings.Reload();
    EXPECT_EQ(settings.START_DATE.GetTimestampMilliseconds(), 1000);
    EXPECT_EQ(settings.END_DATE.GetTimestampMilliseconds(), -1000);
    settings.Save();
    const auto json = Common::JsonUtility::FromFile("storage/settings/sonny_settings.json");
    EXPECT_EQ(json.Get<std::int64_t>("START_DATE_MS"), 1000);
    EXPECT_EQ(json.Get<std::int64_t>("END_DATE_MS"), -1000);
    EXPECT_FALSE(json.Contains("START_DATE"));
    EXPECT_FALSE(json.Contains("END_DATE"));
    EXPECT_EQ(json.Get<int>("input"), 20);
}
