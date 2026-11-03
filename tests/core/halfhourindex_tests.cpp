
#include "core/engine.hpp"
#include "core/transformer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

class HalfHourIndex_Test : public ::testing::Test {
protected:
    static constexpr std::size_t WINDOW_LENGTH = 30;
    static constexpr std::size_t QUERY_DIMENSION = WINDOW_LENGTH * 3;

    std::filesystem::path tempDirectory_;
    std::filesystem::path indexPath_;
    std::filesystem::path metadataPath_;
    std::filesystem::path normalizationPath_;
    std::filesystem::path recordFixturePath_;

    std::unique_ptr<Core::HalfHourIndex> testIndex_;

    // Known normalization parameters for deterministic tests.
    static Core::NormalizationParameters MakeTestNormalizationParameters() {
        return Core::NormalizationParameters{
            .sample_count = 100,

            .mid_delta_mean = 0.0,
            .mid_delta_stddev = 1.0,

            .spread_delta_mean = 0.0,
            .spread_delta_stddev = 1.0,

            .volume_mean = 0.0,
            .volume_stddev = 1.0
        };
    }

    void SetUp() override {
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();

        tempDirectory_ =
            std::filesystem::temp_directory_path() /
            ("sonny_halfhour_index_tests_" + std::to_string(timestamp));

        std::filesystem::create_directories(tempDirectory_);

        indexPath_ = tempDirectory_ / "test.index";
        metadataPath_ = tempDirectory_ / "test.meta";
        normalizationPath_ = tempDirectory_ / "test.normalization.json";

        recordFixturePath_ =
            std::filesystem::path(SONNY_PROJECT_ROOT) /
            "tests/test_storage/records/2026-09-01.rec";

        OpenIndex();

        // Every test index must have valid normalization parameters.
        testIndex_->SetNormalizationParameters(
            MakeTestNormalizationParameters()
        );
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

    // Normalize a copy of the records before passing them to FAISS.
    void AddRecords(
        const std::vector<Core::MLRecord>& records,
        const std::size_t recordOffset = 0,
        const Core::NormalizationParameters& parameters =
            MakeTestNormalizationParameters()
    ) {
        std::vector<Core::MLRecord> normalizedRecords = records;

        std::span<Core::MLRecord> recordSpan(normalizedRecords);

        Core::NormalizeRecordsVector(recordSpan, parameters);

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

    // Construct a normalized 90-dimensional query through HalfHourIndex.
    // This also verifies that the index's normalization parameters are used.
    std::vector<float> MakeQuery(
        const std::vector<Core::MLRecord>& records,
        const std::size_t firstRecord
    ) const {
        std::span<const Core::MLRecord> window(
            records.data() + firstRecord,
            WINDOW_LENGTH
        );

        return testIndex_->NormalizeQuery(window);
    }

    // Produce deterministic raw records for testing the indexing algorithm.
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
};

// ============================================================ //
// FIXTURE TESTS
// ============================================================ //

// Verify that the immutable test fixtures are present.
TEST_F(HalfHourIndex_Test, CheckedInFixturesExist) {
    const auto corpusPath =
        std::filesystem::path(SONNY_PROJECT_ROOT) /
        "tests/test_storage/corpus/2026-09-01.corpus";

    EXPECT_TRUE(std::filesystem::is_regular_file(corpusPath));
    EXPECT_TRUE(std::filesystem::is_regular_file(recordFixturePath_));
}

// ============================================================ //
// QUERY TESTS
// ============================================================ //

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

// Query normalization requires exactly 30 MLRecords.
TEST_F(HalfHourIndex_Test, NormalizeQueryRejectsIncorrectRecordCount) {
    const auto records = MakeRecords(WINDOW_LENGTH - 1);

    std::span<const Core::MLRecord> recordSpan(records);

    EXPECT_THROW(
        testIndex_->NormalizeQuery(recordSpan),
        std::invalid_argument
    );
}

// Verify that normalization produces the expected feature ordering.
TEST_F(HalfHourIndex_Test, NormalizeQueryProducesCorrectFeatureOrder) {
    const auto records = MakeRecords(WINDOW_LENGTH);

    const auto query = MakeQuery(records, 0);

    ASSERT_EQ(query.size(), QUERY_DIMENSION);

    // First record:
    // mid_delta = 1.0
    // spread_delta = 0.25
    // volume = 0.5
    //
    // Mean = 0 and standard deviation = 1 for all features.
    // Volume is transformed using log1p before normalization.

    EXPECT_FLOAT_EQ(query[0], 1.0F);
    EXPECT_FLOAT_EQ(query[1], 0.25F);
    EXPECT_FLOAT_EQ(
        query[2],
        static_cast<float>(std::log1p(0.5))
    );
}

// ============================================================ //
// INDEXING TESTS
// ============================================================ //

// Fewer than 30 records cannot produce a complete search window.
TEST_F(HalfHourIndex_Test, IgnoresInsufficientRecords) {
    const auto records = MakeRecords(WINDOW_LENGTH - 1);

    AddRecords(records);

    const std::vector<float> query(QUERY_DIMENSION, 0.0F);
    const auto results = QueryIndex(query);

    EXPECT_TRUE(results.empty());
}

// Exactly 30 records should produce exactly one vector.
TEST_F(HalfHourIndex_Test, IndexesOneCompleteWindow) {
    const auto records = MakeRecords(WINDOW_LENGTH);

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
    const auto records = MakeRecords(32);

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
    const auto records = MakeRecords(35);

    AddRecords(records);

    const auto query = MakeQuery(records, 0);

    EXPECT_EQ(QueryIndex(query, 2).size(), 2U);
    EXPECT_TRUE(QueryIndex(query, 0).empty());
}

// ============================================================ //
// NORMALIZATION PERSISTENCE TESTS
// ============================================================ //

// Index vectors, metadata, and normalization parameters
// should survive a complete disk round trip.
TEST_F(HalfHourIndex_Test, PersistsIndexMetadataAndNormalization) {
    auto parameters = MakeTestNormalizationParameters();

    // Use distinctive parameters so we can verify that the exact
    // normalization configuration survives JSON serialization.
    parameters.mid_delta_mean = 2.5;
    parameters.mid_delta_stddev = 3.5;

    parameters.spread_delta_mean = 1.5;
    parameters.spread_delta_stddev = 2.5;

    parameters.volume_mean = 0.75;
    parameters.volume_stddev = 1.25;

    testIndex_->SetNormalizationParameters(parameters);

    const auto records = MakeRecords(31);

    AddRecords(records, 300, parameters);

    // Query the second overlapping window.
    const auto queryBeforeSave = MakeQuery(records, 1);

    // Destroying an updated index saves all three files.
    testIndex_.reset();

    ASSERT_TRUE(std::filesystem::is_regular_file(indexPath_));
    ASSERT_TRUE(std::filesystem::is_regular_file(metadataPath_));
    ASSERT_TRUE(std::filesystem::is_regular_file(normalizationPath_));

    // Reopen the same files using the test-only constructor.
    // This should load the normalization parameters from JSON.
    OpenIndex();

    const auto queryAfterLoad = MakeQuery(records, 1);

    ASSERT_EQ(queryAfterLoad.size(), QUERY_DIMENSION);

    // Verify that the loaded normalization parameters produce
    // exactly the same query as the original parameters.
    EXPECT_EQ(queryAfterLoad, queryBeforeSave);

    const auto results = QueryIndex(queryAfterLoad, 2);

    ASSERT_EQ(results.size(), 2U);

    EXPECT_NEAR(results[0].distance, 0.0F, 1e-6F);
    EXPECT_EQ(results[0].recordIndex, 301U);

    const std::array<char, 8> expectedTradingDay{
        '2', '0', '2', '6', '0', '9', '0', '1'
    };

    EXPECT_EQ(results[0].tradingDayId, expectedTradingDay);
}

// An existing index without its normalization JSON must not be usable.
TEST_F(HalfHourIndex_Test, RejectsIndexWithoutNormalizationParameters) {
    // Save the initial empty index and its normalization parameters.
    testIndex_.reset();

    ASSERT_TRUE(std::filesystem::is_regular_file(indexPath_));
    ASSERT_TRUE(std::filesystem::is_regular_file(normalizationPath_));

    // Simulate a legacy index or missing normalization file.
    std::filesystem::remove(normalizationPath_);

    EXPECT_THROW(
        OpenIndex(),
        std::runtime_error
    );
}
