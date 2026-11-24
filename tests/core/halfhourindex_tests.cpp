// halfhourindex_tests.cpp
// Tests for HalfHourMultiIndex (and the HalfHourIndex base it builds on): pathing, the vectors each
// sub-index stores, querying, persistence, and the input-window helpers in inputconfig.hpp.

#include "core/halfhourindex.hpp"
#include "core/halfhourmultiindex.hpp"
#include "core/inputconfig.hpp"
#include "core/transformer.hpp"

#include <gtest/gtest.h>

#include <faiss/IndexFlat.h>
#include <faiss/index_io.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

/**
 * @brief Fixture for the index tests. HalfHourIndex befriends this exact class name, so helpers that
 * need private members (the path builders) live here; TEST_F bodies are subclasses and cannot reach them.
 *
 * Every test runs inside its own temporary working directory: the indexes use relative storage/
 * paths, so this exercises the real path layout without touching the project's storage directory.
 */
class HalfHourIndex_Test : public ::testing::Test {
protected:
    // MEMBERS

    static constexpr std::size_t WINDOW = Core::INPUT_WINDOW_CONFIGURATIONS.front();
    static constexpr std::size_t SEGMENT_START = 0;

    std::filesystem::path PreviousWorkingDirectory;
    std::filesystem::path TempDirectory;
    std::filesystem::path RecordFixturePath;

    // METHODS

    void SetUp() override {
        RecordFixturePath =
            std::filesystem::path(SONNY_PROJECT_ROOT) / "tests/test_storage/records/2026-09-01.rec";

        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        TempDirectory = std::filesystem::temp_directory_path() /
            ("sonny_halfhour_index_tests_" + std::to_string(timestamp));

        std::filesystem::create_directories(TempDirectory);
        PreviousWorkingDirectory = std::filesystem::current_path();
        std::filesystem::current_path(TempDirectory);
    }

    void TearDown() override {
        std::filesystem::current_path(PreviousWorkingDirectory);
        std::filesystem::remove_all(TempDirectory);
    }

    // === PRIVATE PATH ACCESS (friend only) === //

    static std::filesystem::path IndexPath(
        const std::size_t second,
        const std::size_t window,
        const std::optional<std::string_view> type
    ) {
        return Core::HalfHourIndex::GetIndexPath(second, window, type);
    }

    static std::filesystem::path MetadataPath(
        const std::size_t second,
        const std::size_t window,
        const std::optional<std::string_view> type
    ) {
        return Core::HalfHourIndex::GetMetadataPath(second, window, type);
    }

    static std::filesystem::path NormalizationPath(
        const std::size_t second,
        const std::size_t window,
        const std::optional<std::string_view> type
    ) {
        return Core::HalfHourIndex::GetNormalizationPath(second, window, type);
    }

    static std::string HalfHourName(const std::size_t second) {
        return Core::HalfHourIndex::GetHalfHourName(second);
    }

    // === HELPERS === //

    static Core::NormalizationParameters MakeParameters(const double mean = 0.0, const double stddev = 1.0) {
        return Core::NormalizationParameters{
            .SampleCount = 100,
            .MidDeltaMean = mean,
            .MidDeltaStddev = stddev
        };
    }

    /**
     * @brief Deterministic signed records with mixed directions (including flat seconds) and varied sizes,
     * so overlapping windows are distinct in both the magnitude and the direction index.
     */
    static std::vector<Core::MLRecord> MakeRecords(const std::size_t count, std::uint32_t seed = 12345) {
        std::vector<Core::MLRecord> records;
        records.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            seed = seed * 1103515245u + 12345u;
            const int direction = static_cast<int>((seed >> 16) % 3) - 1;
            const float size = 0.01f * static_cast<float>(1 + ((seed >> 8) % 50));

            records.push_back(Core::MLRecord{.MidDelta = static_cast<float>(direction) * size});
        }

        return records;
    }

    static std::vector<Core::MLRecord> LoadRecordFixture(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const std::streamsize size = file.tellg();
        std::vector<Core::MLRecord> records(static_cast<std::size_t>(size) / sizeof(Core::MLRecord));

        file.seekg(0);
        file.read(reinterpret_cast<char*>(records.data()), size);

        return records;
    }

    /**
     * @brief Builds and saves (by destruction) a multi-index from records at the given record offset.
     */
    void BuildIndex(
        const std::vector<Core::MLRecord>& records,
        const std::size_t recordOffset = 0,
        const Core::NormalizationParameters& parameters = MakeParameters(),
        const std::size_t window = WINDOW
    ) {
        Core::HalfHourMultiIndex index(SEGMENT_START, window);
        index.SetNormalizationParameters(parameters);

        std::vector<Core::MLRecord> copy = records;
        index.AddVectorsToIndex(std::span<Core::MLRecord>(copy), RecordFixturePath, recordOffset);
    }

    /**
     * @brief Reads every vector stored in a saved FAISS index.
     */
    static std::vector<std::vector<float>> ReadStoredVectors(const std::filesystem::path& path) {
        std::unique_ptr<faiss::Index> index(faiss::read_index(path.string().c_str()));
        std::vector<std::vector<float>> vectors(static_cast<std::size_t>(index->ntotal));

        for (faiss::idx_t i = 0; i < index->ntotal; ++i) {
            vectors[static_cast<std::size_t>(i)].resize(static_cast<std::size_t>(index->d));
            index->reconstruct(i, vectors[static_cast<std::size_t>(i)].data());
        }

        return vectors;
    }

    static std::span<const Core::MLRecord> Window(
        const std::vector<Core::MLRecord>& records,
        const std::size_t first,
        const std::size_t window = WINDOW
    ) {
        return std::span<const Core::MLRecord>(records).subspan(first, window);
    }

    // MEMBERS

    static constexpr std::array<char, 8> FIXTURE_DAY{'2', '0', '2', '6', '0', '9', '0', '1'};
};

// ============================================================ //
// FIXTURES AND LAYOUT
// ============================================================ //

TEST_F(HalfHourIndex_Test, CheckedInFixturesExist) {
    const auto corpusPath =
        std::filesystem::path(SONNY_PROJECT_ROOT) / "tests/test_storage/corpus/2026-09-01.corpus";

    EXPECT_TRUE(std::filesystem::is_regular_file(corpusPath));
    EXPECT_TRUE(std::filesystem::is_regular_file(RecordFixturePath));
}

TEST_F(HalfHourIndex_Test, RecordFixtureUsesTheCurrentRecordLayout) {
    static_assert(sizeof(Core::MLRecord) == 4);

    // One MidDelta per second of the 6.5-hour regular session.
    ASSERT_EQ(std::filesystem::file_size(RecordFixturePath), 23400u * sizeof(Core::MLRecord));

    const std::vector<Core::MLRecord> records = LoadRecordFixture(RecordFixturePath);

    // The first record is the "no previous second" sentinel; every value must be finite.
    EXPECT_FLOAT_EQ(records.front().MidDelta, 0.0f);
    for (const Core::MLRecord& record : records) {
        ASSERT_TRUE(std::isfinite(record.MidDelta));
    }
}

// ============================================================ //
// PATHS
// ============================================================ //

TEST_F(HalfHourIndex_Test, PathsSeparateWindowsAndSubIndexTypes) {
    using Core::HalfHourIndex;

    EXPECT_EQ(IndexPath(0, 30, HalfHourIndex::INX_TYPE_MAGNITUDE), std::filesystem::path("storage/indexes/30s/m/0930-1000.index"));
    EXPECT_EQ(IndexPath(1800, 20, HalfHourIndex::INX_TYPE_DIRECTION), std::filesystem::path("storage/indexes/20s/d/1000-1030.index"));
    EXPECT_EQ(MetadataPath(0, 45, HalfHourIndex::INX_TYPE_DIRECTION), std::filesystem::path("storage/metadata/45s/d/0930-1000.meta"));
    EXPECT_EQ(
        NormalizationPath(0, 45, HalfHourIndex::INX_TYPE_MAGNITUDE),
        std::filesystem::path("storage/metadata/45s/m/0930-1000.normalization.json")
    );

    // Without a type the old flat layout is kept.
    EXPECT_EQ(IndexPath(0, 30, std::nullopt), std::filesystem::path("storage/indexes/30s/0930-1000.index"));
}

TEST_F(HalfHourIndex_Test, HalfHourNamesCoverTheSessionAndRejectAfterTheClose) {
    EXPECT_EQ(HalfHourName(0), "0930-1000");
    EXPECT_EQ(HalfHourName(1799), "0930-1000");
    EXPECT_EQ(HalfHourName(1800), "1000-1030");
    EXPECT_EQ(HalfHourName(12 * 1800), "1530-1600");
    EXPECT_THROW(HalfHourName(13 * 1800), std::out_of_range);
}

// ============================================================ //
// STORED VECTORS
// ============================================================ //

TEST_F(HalfHourIndex_Test, MagnitudeIndexStoresNormalizedAbsoluteDeltas) {
    const Core::NormalizationParameters parameters = MakeParameters(0.1, 0.05);
    const std::vector<Core::MLRecord> records = MakeRecords(WINDOW + 2);

    BuildIndex(records, 0, parameters);

    const auto vectors = ReadStoredVectors(IndexPath(SEGMENT_START, WINDOW, Core::HalfHourIndex::INX_TYPE_MAGNITUDE));
    ASSERT_EQ(vectors.size(), 3u);

    for (std::size_t v = 0; v < vectors.size(); ++v) {
        ASSERT_EQ(vectors[v].size(), WINDOW);

        for (std::size_t i = 0; i < WINDOW; ++i) {
            const double absolute = std::abs(records[v + i].MidDelta);
            const float expected = static_cast<float>((absolute - parameters.MidDeltaMean) / parameters.MidDeltaStddev);
            EXPECT_FLOAT_EQ(vectors[v][i], expected) << "vector " << v << ", second " << i;
        }
    }
}

TEST_F(HalfHourIndex_Test, DirectionVectorsUseWeightsBasedOnTheirConfiguredDimension) {
    const std::vector<Core::MLRecord> records = MakeRecords(WINDOW + 2);

    BuildIndex(records, 0, MakeParameters(5.0, 3.0));

    const auto vectors = ReadStoredVectors(IndexPath(SEGMENT_START, WINDOW, Core::HalfHourIndex::INX_TYPE_DIRECTION));
    ASSERT_EQ(vectors.size(), 3u);

    for (std::size_t v = 0; v < vectors.size(); ++v) {
        for (std::size_t i = 0; i < WINDOW; ++i) {
            const float delta = records[v + i].MidDelta;
            const float direction = delta > 0.0f ? 1.0f : (delta < 0.0f ? -1.0f : 0.0f);
            const double weight = 0.4 + 1.8 * (static_cast<double>(i) / static_cast<double>(WINDOW - 1));
            const float expected = direction * static_cast<float>(std::sqrt(weight));
            EXPECT_FLOAT_EQ(vectors[v][i], expected) << "vector " << v << ", second " << i;
        }
    }
}

TEST_F(HalfHourIndex_Test, LongestConfiguredDirectionIndexUsesMatchingTemporalWeightsForStorageAndQueries) {
    constexpr std::size_t window = Core::MAX_INPUT_WINDOW_SECONDS;
    std::vector<Core::MLRecord> records;
    records.reserve(window);

    for (std::size_t position = 0; position < window; ++position) {
        records.push_back(Core::MLRecord{
            .MidDelta = position % 2 == 0 ? 1.0F : -1.0F
        });
    }

    BuildIndex(records, 0, MakeParameters(), window);

    const auto directionVectors = ReadStoredVectors(
        IndexPath(SEGMENT_START, window, Core::HalfHourIndex::INX_TYPE_DIRECTION)
    );
    ASSERT_EQ(directionVectors.size(), 1u);
    ASSERT_EQ(directionVectors.front().size(), window);

    double totalWeight = 0.0;
    for (std::size_t position = 0; position < window; ++position) {
        const double weight = 0.4 + 1.8 * (static_cast<double>(position) / static_cast<double>(window - 1));
        totalWeight += weight;
        const float direction = records[position].MidDelta > 0.0F ? 1.0F : -1.0F;
        const float expected = direction * static_cast<float>(std::sqrt(weight));
        EXPECT_FLOAT_EQ(directionVectors.front()[position], expected) << "position " << position;
    }

    EXPECT_NEAR(totalWeight / static_cast<double>(window), 1.3, 1e-12);

    const float oldestScale = static_cast<float>(std::sqrt(0.4));
    const float newestScale = static_cast<float>(std::sqrt(2.2));
    EXPECT_FLOAT_EQ(directionVectors.front().front(), oldestScale);
    EXPECT_FLOAT_EQ(directionVectors.front().back(), records.back().MidDelta * newestScale);

    const auto magnitudeVectors = ReadStoredVectors(
        IndexPath(SEGMENT_START, window, Core::HalfHourIndex::INX_TYPE_MAGNITUDE)
    );
    ASSERT_EQ(magnitudeVectors.size(), 1u);
    ASSERT_EQ(magnitudeVectors.front().size(), window);
    for (std::size_t position = 0; position < window; ++position) {
        EXPECT_FLOAT_EQ(magnitudeVectors.front()[position], 1.0F);
    }

    std::vector<Core::MLRecord> query = records;
    constexpr std::array<std::size_t, 5> flippedPositions{
        0, window / 4, window / 2, 3 * window / 4, window - 1
    };
    double expectedSquaredDistance = 0.0;
    for (const std::size_t position : flippedPositions) {
        query[position].MidDelta = -query[position].MidDelta;
        const double weight = 0.4 + 1.8 * (static_cast<double>(position) / static_cast<double>(window - 1));
        expectedSquaredDistance += 4.0 * weight;
    }

    const Core::HalfHourMultiIndex index(SEGMENT_START, window);
    const auto results = index.Query(query, 1);
    ASSERT_EQ(results.MagnitudeResults.size(), 1u);
    ASSERT_EQ(results.DirectionResults.size(), 1u);
    EXPECT_FLOAT_EQ(results.MagnitudeResults.front().Distance, 0.0F);
    EXPECT_NEAR(results.DirectionResults.front().Distance, expectedSquaredDistance, 1e-4);
}

TEST_F(HalfHourIndex_Test, MetadataTracksRecordOffsetsAndTradingDay) {
    const std::vector<Core::MLRecord> records = MakeRecords(WINDOW + 2);
    BuildIndex(records, 1800);

    const Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);

    // The second overlapping window starts one record after the segment offset.
    const auto results = index.Query(Window(records, 1), 1);

    ASSERT_EQ(results.MagnitudeResults.size(), 1u);
    ASSERT_EQ(results.DirectionResults.size(), 1u);

    for (const auto& result : {results.MagnitudeResults.front(), results.DirectionResults.front()}) {
        EXPECT_NEAR(result.Distance, 0.0f, 1e-6f);
        EXPECT_EQ(result.RecordIndex, 1801u);
        EXPECT_EQ(result.TradingDayId, FIXTURE_DAY);
    }
}

// ============================================================ //
// QUERYING
// ============================================================ //

TEST_F(HalfHourIndex_Test, MagnitudeIgnoresSignAndDirectionIgnoresSize) {
    const std::vector<Core::MLRecord> records = MakeRecords(WINDOW);
    BuildIndex(records);

    const Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);

    // Same sizes, every direction flipped: identical magnitudes, different directions.
    std::vector<Core::MLRecord> flipped = records;
    for (Core::MLRecord& record : flipped) record.MidDelta = -record.MidDelta;

    const auto flippedResults = index.Query(flipped, 1);
    EXPECT_NEAR(flippedResults.MagnitudeResults.front().Distance, 0.0f, 1e-6f);
    EXPECT_GT(flippedResults.DirectionResults.front().Distance, 0.0f);

    // Same directions, every size tripled: identical directions, different magnitudes.
    std::vector<Core::MLRecord> scaled = records;
    for (Core::MLRecord& record : scaled) record.MidDelta *= 3.0f;

    const auto scaledResults = index.Query(scaled, 1);
    EXPECT_GT(scaledResults.MagnitudeResults.front().Distance, 0.0f);
    EXPECT_NEAR(scaledResults.DirectionResults.front().Distance, 0.0f, 1e-6f);
}

TEST_F(HalfHourIndex_Test, TiedMatchesPreferTheDaysAddedFirst) {
    // Setup adds days newest first so that FAISS, which breaks distance ties by the lowest vector ID,
    // resolves tied matches to the most recent days. Pin that behaviour here.
    const std::vector<Core::MLRecord> pattern = MakeRecords(WINDOW);
    const std::array<std::filesystem::path, 4> newestFirst{
        "storage/records/2026-09-04.rec",
        "storage/records/2026-09-03.rec",
        "storage/records/2026-09-02.rec",
        "storage/records/2026-09-01.rec"
    };

    {
        Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);
        index.SetNormalizationParameters(MakeParameters());

        for (std::size_t day = 0; day < newestFirst.size(); ++day) {
            // Same directions every day; sizes differ so only the direction index ties.
            std::vector<Core::MLRecord> records = pattern;
            for (Core::MLRecord& record : records) record.MidDelta *= static_cast<float>(day + 1);

            index.AddVectorsToIndex(std::span<Core::MLRecord>(records), newestFirst[day], 0);
        }
    }

    const Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);
    const auto results = index.Query(pattern, 2);

    // All four days tie at distance 0; the cap keeps the two newest, newest first.
    ASSERT_EQ(results.DirectionResults.size(), 2u);
    EXPECT_FLOAT_EQ(results.DirectionResults[0].Distance, 0.0f);
    EXPECT_FLOAT_EQ(results.DirectionResults[1].Distance, 0.0f);

    const std::array<char, 8> newest{'2', '0', '2', '6', '0', '9', '0', '4'};
    const std::array<char, 8> secondNewest{'2', '0', '2', '6', '0', '9', '0', '3'};
    EXPECT_EQ(results.DirectionResults[0].TradingDayId, newest);
    EXPECT_EQ(results.DirectionResults[1].TradingDayId, secondNewest);
}

TEST_F(HalfHourIndex_Test, EmptyIndexReturnsNoResults) {
    Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);
    index.SetNormalizationParameters(MakeParameters());

    const auto results = index.Query(MakeRecords(WINDOW), 5);

    EXPECT_TRUE(results.MagnitudeResults.empty());
    EXPECT_TRUE(results.DirectionResults.empty());
}

TEST_F(HalfHourIndex_Test, QueryRejectsIncorrectRecordCount) {
    Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);
    index.SetNormalizationParameters(MakeParameters());

    EXPECT_THROW(index.Query(MakeRecords(WINDOW - 1), 5), std::invalid_argument);
    EXPECT_THROW(index.Query(MakeRecords(WINDOW + 1), 5), std::invalid_argument);
}

TEST_F(HalfHourIndex_Test, QueryRespectsMaxResults) {
    const std::vector<Core::MLRecord> records = MakeRecords(WINDOW + 5);
    BuildIndex(records);

    const Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);

    const auto two = index.Query(Window(records, 0), 2);
    EXPECT_EQ(two.MagnitudeResults.size(), 2u);
    EXPECT_EQ(two.DirectionResults.size(), 2u);

    // Asking for more than exist returns every vector.
    const auto all = index.Query(Window(records, 0), 100);
    EXPECT_EQ(all.MagnitudeResults.size(), 6u);
    EXPECT_EQ(all.DirectionResults.size(), 6u);

    const auto none = index.Query(Window(records, 0), 0);
    EXPECT_TRUE(none.MagnitudeResults.empty());
    EXPECT_TRUE(none.DirectionResults.empty());
}

TEST_F(HalfHourIndex_Test, BuildsFromTheRecordFixtureAndFindsItsOwnWindows) {
    const std::vector<Core::MLRecord> day = LoadRecordFixture(RecordFixturePath);

    // Mirror Setup::ExecuteUpdateIndexes for segment 0: 1800 window starts plus window padding.
    const std::size_t segmentSize = 1800 + WINDOW - 1;
    std::vector<Core::MLRecord> segment(day.begin(), day.begin() + static_cast<std::ptrdiff_t>(segmentSize));

    Core::NormalizationAccumulator accumulator{};
    for (Core::MLRecord record : segment) {
        record.MidDelta = std::abs(record.MidDelta);
        accumulator.AddRecord(record);
    }

    BuildIndex(segment, 0, accumulator.GetParameters());

    const Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);

    for (const std::size_t start : {std::size_t{0}, std::size_t{500}, std::size_t{1799}}) {
        const auto results = index.Query(Window(day, start), 64);

        ASSERT_FALSE(results.MagnitudeResults.empty());
        EXPECT_NEAR(results.MagnitudeResults.front().Distance, 0.0f, 1e-6f);
        EXPECT_EQ(results.MagnitudeResults.front().TradingDayId, FIXTURE_DAY);

        // Direction vectors are coarse, so the window may tie with others at distance 0;
        // it must be among them.
        bool foundSelf = false;
        for (const auto& result : results.DirectionResults) {
            EXPECT_GE(result.Distance, results.DirectionResults.front().Distance);
            if (result.Distance == 0.0f && result.RecordIndex == start) foundSelf = true;
        }
        EXPECT_NEAR(results.DirectionResults.front().Distance, 0.0f, 1e-6f);
        EXPECT_TRUE(foundSelf) << "window starting at " << start;
    }
}

TEST_F(HalfHourIndex_Test, BuildsAndQueriesEveryInputWindowConfiguration) {
    for (const std::size_t window : Core::INPUT_WINDOW_CONFIGURATIONS) {
        const std::vector<Core::MLRecord> records = MakeRecords(window + 3, static_cast<std::uint32_t>(window));
        BuildIndex(records, 50, MakeParameters(0.1, 0.05), window);

        EXPECT_TRUE(Core::HalfHourMultiIndex::FilesExist(SEGMENT_START, window)) << window << "s";

        const Core::HalfHourMultiIndex index(SEGMENT_START, window);
        const auto results = index.Query(Window(records, 2, window), 1);

        ASSERT_EQ(results.MagnitudeResults.size(), 1u) << window << "s";
        EXPECT_NEAR(results.MagnitudeResults.front().Distance, 0.0f, 1e-5f);
        EXPECT_EQ(results.MagnitudeResults.front().RecordIndex, 52u);
        EXPECT_EQ(results.DirectionResults.front().RecordIndex, 52u);
    }
}

// ============================================================ //
// INDEXING RULES
// ============================================================ //

TEST_F(HalfHourIndex_Test, IgnoresInsufficientRecords) {
    BuildIndex(MakeRecords(WINDOW - 1));

    const Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);
    const auto results = index.Query(MakeRecords(WINDOW), 5);

    EXPECT_TRUE(results.MagnitudeResults.empty());
    EXPECT_TRUE(results.DirectionResults.empty());
}

TEST_F(HalfHourIndex_Test, RejectsVectorsBeforeNormalizationParameters) {
    Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);
    std::vector<Core::MLRecord> records = MakeRecords(WINDOW);

    EXPECT_THROW(
        index.AddVectorsToIndex(std::span<Core::MLRecord>(records), RecordFixturePath, 0),
        std::logic_error
    );
}

TEST_F(HalfHourIndex_Test, RejectsNormalizationChangesAfterIndexing) {
    Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);
    index.SetNormalizationParameters(MakeParameters());

    std::vector<Core::MLRecord> records = MakeRecords(WINDOW);
    index.AddVectorsToIndex(std::span<Core::MLRecord>(records), RecordFixturePath, 0);

    EXPECT_THROW(index.SetNormalizationParameters(MakeParameters(1.0, 2.0)), std::logic_error);
}

// ============================================================ //
// PERSISTENCE
// ============================================================ //

TEST_F(HalfHourIndex_Test, PersistsBothSubIndexesButOnlyMagnitudeNormalization) {
    using Core::HalfHourIndex;

    Core::NormalizationParameters parameters = MakeParameters(2.5, 3.5);
    const std::vector<Core::MLRecord> records = MakeRecords(WINDOW + 1);

    BuildIndex(records, 300, parameters);

    for (const std::string_view type : {HalfHourIndex::INX_TYPE_MAGNITUDE, HalfHourIndex::INX_TYPE_DIRECTION}) {
        EXPECT_TRUE(std::filesystem::is_regular_file(IndexPath(SEGMENT_START, WINDOW, type))) << type;
        EXPECT_TRUE(std::filesystem::is_regular_file(MetadataPath(SEGMENT_START, WINDOW, type))) << type;
    }

    const std::filesystem::path magnitudeNormalization = NormalizationPath(SEGMENT_START, WINDOW, HalfHourIndex::INX_TYPE_MAGNITUDE);
    ASSERT_TRUE(std::filesystem::is_regular_file(magnitudeNormalization));
    EXPECT_FALSE(std::filesystem::exists(NormalizationPath(SEGMENT_START, WINDOW, HalfHourIndex::INX_TYPE_DIRECTION)));

    std::ifstream normalizationFile(magnitudeNormalization);
    const std::string normalizationJson(
        (std::istreambuf_iterator<char>(normalizationFile)),
        std::istreambuf_iterator<char>()
    );
    EXPECT_NE(normalizationJson.find(std::format("\"input_window_seconds\":{}", WINDOW)), std::string::npos);
    EXPECT_NE(normalizationJson.find("\"mid_delta_mean\":2.5"), std::string::npos);
    EXPECT_NE(normalizationJson.find("\"mid_delta_stddev\":3.5"), std::string::npos);

    EXPECT_TRUE(Core::HalfHourMultiIndex::FilesExist(SEGMENT_START, WINDOW));

    // Reloading uses the persisted normalization, so the same raw window matches exactly.
    const Core::HalfHourMultiIndex reloaded(SEGMENT_START, WINDOW);
    const auto results = reloaded.Query(Window(records, 1), 2);

    ASSERT_EQ(results.MagnitudeResults.size(), 2u);
    EXPECT_NEAR(results.MagnitudeResults.front().Distance, 0.0f, 1e-6f);
    EXPECT_EQ(results.MagnitudeResults.front().RecordIndex, 301u);
    EXPECT_EQ(results.DirectionResults.front().RecordIndex, 301u);
}

TEST_F(HalfHourIndex_Test, LoadingAnIndexDoesNotRewriteItsFiles) {
    BuildIndex(MakeRecords(WINDOW + 1));

    const std::filesystem::path path = IndexPath(SEGMENT_START, WINDOW, Core::HalfHourIndex::INX_TYPE_DIRECTION);
    const auto before = std::filesystem::last_write_time(path);

    {
        const Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);
        index.Query(MakeRecords(WINDOW), 1);
    }

    EXPECT_EQ(std::filesystem::last_write_time(path), before);
}

TEST_F(HalfHourIndex_Test, BothIndexesRetainEveryTradingDayAfterReload) {
    constexpr std::size_t window = Core::DEFAULT_INPUT_WINDOW_SECONDS;
    const std::array<std::filesystem::path, 5> days{
        "2026-08-31.rec", "2026-09-01.rec", "2026-09-02.rec", "2026-09-03.rec", "2026-09-04.rec"
    };
    std::vector<Core::MLRecord> records = MakeRecords(window);

    {
        Core::HalfHourMultiIndex index(SEGMENT_START, window);
        index.SetNormalizationParameters(MakeParameters());
        for (const auto& day : days) {
            index.AddVectorsToIndex(records, day, 0);
        }
    }

    ASSERT_TRUE(Core::HalfHourMultiIndex::FilesExist(SEGMENT_START, window));
    const Core::HalfHourMultiIndex reloaded(SEGMENT_START, window);
    const auto results = reloaded.Query(records, days.size());

    ASSERT_EQ(results.MagnitudeResults.size(), days.size());
    ASSERT_EQ(results.DirectionResults.size(), days.size());
    for (std::size_t i = 0; i < days.size(); ++i) {
        const auto expectedDay = Core::HalfHourIndex::ToShortLocationArray(days[i]);
        EXPECT_EQ(results.MagnitudeResults[i].TradingDayId, expectedDay);
        EXPECT_EQ(results.DirectionResults[i].TradingDayId, expectedDay);
        EXPECT_NEAR(results.MagnitudeResults[i].Distance, 0.0F, 1e-5F);
        EXPECT_NEAR(results.DirectionResults[i].Distance, 0.0F, 1e-5F);
    }
}

TEST_F(HalfHourIndex_Test, FilesExistRequiresEveryFileOfBothSubIndexes) {
    using Core::HalfHourIndex;

    EXPECT_FALSE(Core::HalfHourMultiIndex::FilesExist(SEGMENT_START, WINDOW));

    BuildIndex(MakeRecords(WINDOW));
    ASSERT_TRUE(Core::HalfHourMultiIndex::FilesExist(SEGMENT_START, WINDOW));

    // The direction index is complete without a normalization file.
    EXPECT_TRUE(HalfHourIndex::FilesExist(SEGMENT_START, WINDOW, HalfHourIndex::INX_TYPE_DIRECTION));

    // Another window or segment is unaffected.
    EXPECT_FALSE(Core::HalfHourMultiIndex::FilesExist(SEGMENT_START, Core::INPUT_WINDOW_CONFIGURATIONS[1]));
    EXPECT_FALSE(Core::HalfHourMultiIndex::FilesExist(1800, WINDOW));

    const std::array<std::filesystem::path, 5> requiredFiles{
        IndexPath(SEGMENT_START, WINDOW, HalfHourIndex::INX_TYPE_MAGNITUDE),
        MetadataPath(SEGMENT_START, WINDOW, HalfHourIndex::INX_TYPE_MAGNITUDE),
        NormalizationPath(SEGMENT_START, WINDOW, HalfHourIndex::INX_TYPE_MAGNITUDE),
        IndexPath(SEGMENT_START, WINDOW, HalfHourIndex::INX_TYPE_DIRECTION),
        MetadataPath(SEGMENT_START, WINDOW, HalfHourIndex::INX_TYPE_DIRECTION)
    };

    for (const std::filesystem::path& file : requiredFiles) {
        const std::filesystem::path aside = file.string() + ".aside";
        std::filesystem::rename(file, aside);

        EXPECT_FALSE(Core::HalfHourMultiIndex::FilesExist(SEGMENT_START, WINDOW)) << "missing " << file;

        std::filesystem::rename(aside, file);
    }
}

TEST_F(HalfHourIndex_Test, RejectsIndexWithAnotherInputWindowDimension) {
    const std::filesystem::path path = IndexPath(SEGMENT_START, WINDOW, Core::HalfHourIndex::INX_TYPE_MAGNITUDE);
    std::filesystem::create_directories(path.parent_path());

    faiss::IndexFlatL2 incompatibleIndex(static_cast<int>(WINDOW) - 2);
    faiss::write_index(&incompatibleIndex, path.string().c_str());

    EXPECT_THROW(Core::HalfHourMultiIndex(SEGMENT_START, WINDOW), std::runtime_error);
}

TEST_F(HalfHourIndex_Test, RejectsMagnitudeIndexWithoutNormalizationParameters) {
    BuildIndex(MakeRecords(WINDOW));

    std::filesystem::remove(NormalizationPath(SEGMENT_START, WINDOW, Core::HalfHourIndex::INX_TYPE_MAGNITUDE));

    EXPECT_THROW(Core::HalfHourMultiIndex(SEGMENT_START, WINDOW), std::runtime_error);
}

TEST_F(HalfHourIndex_Test, RejectsNormalizationFromAnotherInputWindow) {
    BuildIndex(MakeRecords(WINDOW));

    const std::filesystem::path path = NormalizationPath(SEGMENT_START, WINDOW, Core::HalfHourIndex::INX_TYPE_MAGNITUDE);

    std::ifstream input(path);
    std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();

    const std::string configured = std::format("\"input_window_seconds\":{}", WINDOW);
    const std::size_t position = json.find(configured);
    ASSERT_NE(position, std::string::npos);
    json.replace(position, configured.size(), std::format("\"input_window_seconds\":{}", Core::MAX_INPUT_WINDOW_SECONDS));

    std::ofstream output(path, std::ios::trunc);
    output << json;
    output.close();

    EXPECT_THROW(Core::HalfHourMultiIndex(SEGMENT_START, WINDOW), std::runtime_error);
}

// ============================================================ //
// INPUT WINDOW HELPERS
// ============================================================ //

TEST(InputConfigTests, InputVectorDimensionIsOneFeaturePerSecond) {
    for (const std::size_t window : Core::INPUT_WINDOW_CONFIGURATIONS) {
        EXPECT_EQ(Core::InputVectorDimension(window), window);
        EXPECT_TRUE(Core::IsSupportedInputWindow(window));
    }

    for (const std::size_t unsupported : {0u, 10u, 15u, 25u, 60u}) {
        EXPECT_FALSE(Core::IsSupportedInputWindow(unsupported)) << unsupported;
    }
}

TEST(InputConfigTests, EachQueryUsesTheMostRecentConfiguredRecords) {
    std::vector<Core::MLRecord> records;
    for (std::size_t i = 1; i <= Core::MAX_INPUT_WINDOW_SECONDS; ++i) {
        records.push_back(Core::MLRecord{.MidDelta = static_cast<float>(i)});
    }

    for (const std::size_t window : Core::INPUT_WINDOW_CONFIGURATIONS) {
        const std::span<const Core::MLRecord> queryWindow =
            Core::LatestInputWindow(std::span<const Core::MLRecord>(records), window);

        ASSERT_EQ(queryWindow.size(), window);
        EXPECT_FLOAT_EQ(queryWindow.front().MidDelta, static_cast<float>(Core::MAX_INPUT_WINDOW_SECONDS - window + 1));
        EXPECT_FLOAT_EQ(queryWindow.back().MidDelta, static_cast<float>(Core::MAX_INPUT_WINDOW_SECONDS));
    }
}

TEST(InputConfigTests, LatestInputWindowRejectsUnsupportedOrShortInput) {
    const std::vector<Core::MLRecord> records(Core::MAX_INPUT_WINDOW_SECONDS, Core::MLRecord{.MidDelta = 0.0f});
    const std::span<const Core::MLRecord> span(records);

    EXPECT_THROW(Core::LatestInputWindow(span, 25), std::invalid_argument);
    for (const std::size_t window : Core::INPUT_WINDOW_CONFIGURATIONS) {
        EXPECT_THROW(Core::LatestInputWindow(span.first(window - 1), window), std::invalid_argument);
    }
}

TEST(InputConfigTests, InputQueueKeepsOnlyTheMostRecentConfiguredRecords) {
    std::deque<int> records;

    for (std::size_t i = 0; i < Core::LIVE_INPUT_WINDOW_SECONDS + 2; ++i) {
        Core::AppendInputWindowRecord(records, static_cast<int>(i));
    }

    ASSERT_EQ(records.size(), Core::LIVE_INPUT_WINDOW_SECONDS);
    EXPECT_EQ(records.front(), 2);
    EXPECT_EQ(records.back(), static_cast<int>(Core::LIVE_INPUT_WINDOW_SECONDS + 1));
}

TEST(InputConfigTests, HistoricalFutureBeginsAfterConfiguredInputWindow) {
    for (const std::size_t window : Core::INPUT_WINDOW_CONFIGURATIONS) {
        EXPECT_EQ(Core::HistoricalFutureStart(100, window), 100 + window);
    }
}

TEST_F(HalfHourIndex_Test, RebuildAbortsIfDirectionIndexCannotBeDeleted) {
    const auto directionPath = IndexPath(SEGMENT_START, WINDOW, Core::HalfHourIndex::INX_TYPE_DIRECTION);
    const auto metadataPath = MetadataPath(SEGMENT_START, WINDOW, Core::HalfHourIndex::INX_TYPE_DIRECTION);
    BuildIndex(MakeRecords(WINDOW));
    /**
     * @brief Restores the index directory permissions after a deletion-failure test.
     */
    struct RestorePermissions {
        // MEMBERS

        std::filesystem::path Path;
        std::filesystem::perms Permissions;

        // DESTRUCTOR

        ~RestorePermissions() {
            std::error_code ignored;
            std::filesystem::permissions(Path, Permissions, ignored);
        }
    } restore{directionPath.parent_path(), std::filesystem::status(directionPath.parent_path()).permissions()};
    std::filesystem::permissions(restore.Path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    // Some CI hosts run as root; verify the restriction before attempting the rebuild.
    std::error_code probe;
    std::filesystem::remove(directionPath, probe);
    if (!probe) GTEST_SKIP() << "This account bypasses directory write restrictions.";
    bool replacementConstructed = false;
    EXPECT_THROW({
        Core::HalfHourMultiIndex::DeleteFiles(SEGMENT_START, WINDOW);
        replacementConstructed = true;
        Core::HalfHourMultiIndex index(SEGMENT_START, WINDOW);
        index.SetNormalizationParameters(MakeParameters());
        auto records = MakeRecords(WINDOW);
        index.AddVectorsToIndex(records, RecordFixturePath, 0);
    }, std::filesystem::filesystem_error);
    EXPECT_FALSE(replacementConstructed);
    EXPECT_EQ(ReadStoredVectors(directionPath).size(), 1u);
    EXPECT_EQ(std::filesystem::file_size(metadataPath), 12u);
}
