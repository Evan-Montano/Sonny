// halfhourindex_tests.cpp

#include "core/engine.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

class HalfHourIndex_Test : public ::testing::Test {
protected:
    static constexpr std::size_t WINDOW_LENGTH = 30;
    static constexpr std::size_t QUERY_DIMENSION = WINDOW_LENGTH * 3;

    std::filesystem::path tempDirectory_;
    std::filesystem::path indexPath_;
    std::filesystem::path metadataPath_;
    std::filesystem::path recordFixturePath_;

    std::unique_ptr<Core::HalfHourIndex> testIndex_;

    void SetUp() override {
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();

        tempDirectory_ =
            std::filesystem::temp_directory_path() /
            ("sonny_halfhour_index_tests_" + std::to_string(timestamp));

        std::filesystem::create_directories(tempDirectory_);

        indexPath_ = tempDirectory_ / "test.index";
        metadataPath_ = tempDirectory_ / "test.meta";

        recordFixturePath_ =
            std::filesystem::path(SONNY_PROJECT_ROOT) /
            "tests/test_storage/records/2026-09-01.rec";

        OpenIndex();
    }

    void TearDown() override {
        // Destroy the index before deleting its backing files.
        testIndex_.reset();
        std::filesystem::remove_all(tempDirectory_);
    }

    // Must remain a member of the friend class to access
    // HalfHourIndex's private path-based constructor.
    void OpenIndex() {
        testIndex_.reset(
            new Core::HalfHourIndex(indexPath_, metadataPath_)
        );
    }

    void AddRecords(
        std::vector<Core::MLRecord>& records,
        const std::size_t recordOffset = 0
    ) {
        std::span<Core::MLRecord> recordSpan(records);

        testIndex_->AddVectorsToIndex(
            recordSpan,
            recordFixturePath_,
            recordOffset
        );
    }

    auto QueryIndex(
        const std::vector<float>& query,
        const std::size_t maxResults = 5
    ) {
        return testIndex_->Query(query, maxResults);
    }

    // Produce deterministic records for testing the indexing algorithm.
    static std::vector<Core::MLRecord> MakeRecords(
        const std::size_t count
    ) {
        std::vector<Core::MLRecord> records;
        records.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            const float value = static_cast<float>(i + 1);

            Core::MLRecord record{};
            record.mid_delta = value;
            record.spread_delta = value * 0.25F;
            record.volume = value * 0.5F;

            records.push_back(record);
        }

        return records;
    }

    // Construct the exact flattened 90-float query corresponding
    // to a 30-record window.
    static std::vector<float> MakeQuery(
        const std::vector<Core::MLRecord>& records,
        const std::size_t firstRecord
    ) {
        std::vector<float> query;
        query.reserve(QUERY_DIMENSION);

        for (std::size_t i = 0; i < WINDOW_LENGTH; ++i) {
            const Core::MLRecord& record = records[firstRecord + i];

            query.push_back(record.mid_delta);
            query.push_back(record.spread_delta);
            query.push_back(record.volume);
        }

        return query;
    }
};

// Verify that the immutable test fixtures are present.
TEST_F(HalfHourIndex_Test, CheckedInFixturesExist) {
    const auto corpusPath =
        std::filesystem::path(SONNY_PROJECT_ROOT) /
        "tests/test_storage/corpus/2026-09-01.corpus";

    EXPECT_TRUE(std::filesystem::is_regular_file(corpusPath));
    EXPECT_TRUE(std::filesystem::is_regular_file(recordFixturePath_));
}

// A new index should return no results.
TEST_F(HalfHourIndex_Test, EmptyIndexReturnsNoResults) {
    const std::vector<float> query(QUERY_DIMENSION, 0.0F);

    const auto results = QueryIndex(query);

    EXPECT_TRUE(results.empty());
}

// Queries must contain exactly 90 floats.
TEST_F(HalfHourIndex_Test, QueryRejectsIncorrectDimension) {
    const std::vector<float> query(QUERY_DIMENSION - 1, 0.0F);

    EXPECT_THROW(
        QueryIndex(query),
        std::invalid_argument
    );
}

// Fewer than 30 records cannot produce a complete search window.
TEST_F(HalfHourIndex_Test, IgnoresInsufficientRecords) {
    auto records = MakeRecords(WINDOW_LENGTH - 1);

    AddRecords(records);

    const std::vector<float> query(QUERY_DIMENSION, 0.0F);
    const auto results = QueryIndex(query);

    EXPECT_TRUE(results.empty());
}

// Exactly 30 records should produce exactly one vector.
TEST_F(HalfHourIndex_Test, IndexesOneCompleteWindow) {
    auto records = MakeRecords(WINDOW_LENGTH);

    AddRecords(records, 17);

    const auto query = MakeQuery(records, 0);
    const auto results = QueryIndex(query);

    ASSERT_EQ(results.size(), 1U);

    EXPECT_NEAR(results[0].distance, 0.0F, 1e-6F);
    EXPECT_EQ(results[0].recordIndex, 17U);

    const std::array<char, 8> expectedTradingDay{
        '2', '0', '2', '6', '0', '9', '0', '1'
    };

    EXPECT_EQ(results[0].tradingDayId, expectedTradingDay);
}

// N records produce N - 29 overlapping 30-record windows.
TEST_F(HalfHourIndex_Test, IndexesOverlappingWindows) {
    auto records = MakeRecords(32);

    AddRecords(records, 100);

    const auto query = MakeQuery(records, 1);
    const auto results = QueryIndex(query, 1);

    ASSERT_EQ(results.size(), 1U);

    // The matching window starts at record offset 100 + 1.
    EXPECT_NEAR(results[0].distance, 0.0F, 1e-6F);
    EXPECT_EQ(results[0].recordIndex, 101U);
}

// Query should respect the requested result limit.
TEST_F(HalfHourIndex_Test, QueryRespectsMaxResults) {
    auto records = MakeRecords(35);

    AddRecords(records);

    const auto query = MakeQuery(records, 0);

    EXPECT_EQ(QueryIndex(query, 2).size(), 2U);
    EXPECT_TRUE(QueryIndex(query, 0).empty());
}

// Index vectors and their metadata should survive a disk round trip.
TEST_F(HalfHourIndex_Test, PersistsIndexAndMetadata) {
    auto records = MakeRecords(31);

    AddRecords(records, 300);

    // Query the second overlapping window.
    const auto query = MakeQuery(records, 1);

    // Destroying an updated index saves the index and metadata.
    testIndex_.reset();

    ASSERT_TRUE(std::filesystem::is_regular_file(indexPath_));
    ASSERT_TRUE(std::filesystem::is_regular_file(metadataPath_));

    // Reopen the same files using the test-only constructor.
    OpenIndex();

    const auto results = QueryIndex(query, 2);

    ASSERT_EQ(results.size(), 2U);

    EXPECT_NEAR(results[0].distance, 0.0F, 1e-6F);
    EXPECT_EQ(results[0].recordIndex, 301U);

    const std::array<char, 8> expectedTradingDay{
        '2', '0', '2', '6', '0', '9', '0', '1'
    };

    EXPECT_EQ(results[0].tradingDayId, expectedTradingDay);
}