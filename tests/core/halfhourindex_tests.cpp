
// #include "core/halfhourindex.hpp"
// #include "core/transformer.hpp"

// #include <gtest/gtest.h>

// #include <array>
// #include <chrono>
// #include <cmath>
// #include <cstddef>
// #include <filesystem>
// #include <fstream>
// #include <format>
// #include <iterator>
// #include <memory>
// #include <span>
// #include <stdexcept>
// #include <string>
// #include <vector>

// class HalfHourIndex_Test : public ::testing::Test {
// protected:
//     static constexpr std::size_t WINDOW_LENGTH = Core::DEFAULT_INPUT_WINDOW_SECONDS;
//     static constexpr std::size_t QUERY_DIMENSION = Core::InputVectorDimension(WINDOW_LENGTH);

//     std::filesystem::path tempDirectory_;
//     std::filesystem::path indexPath_;
//     std::filesystem::path metadataPath_;
//     std::filesystem::path normalizationPath_;
//     std::filesystem::path recordFixturePath_;

//     std::unique_ptr<Core::HalfHourIndex> testIndex_;

//     // Known normalization parameters for deterministic tests.
//     static Core::NormalizationParameters MakeTestNormalizationParameters() {
//         return Core::NormalizationParameters{
//             .sample_count = 100,

//             .mid_delta_mean = 0.0,
//             .mid_delta_stddev = 1.0
//         };
//     }

//     void SetUp() override {
//         const auto timestamp =
//             std::chrono::steady_clock::now().time_since_epoch().count();

//         tempDirectory_ =
//             std::filesystem::temp_directory_path() /
//             ("sonny_halfhour_index_tests_" + std::to_string(timestamp));

//         std::filesystem::create_directories(tempDirectory_);

//         indexPath_ = tempDirectory_ / "test.index";
//         metadataPath_ = tempDirectory_ / "test.meta";
//         normalizationPath_ = tempDirectory_ / "test.normalization.json";

//         recordFixturePath_ =
//             std::filesystem::path(SONNY_PROJECT_ROOT) /
//             "tests/test_storage/records/2026-09-01.rec";

//         OpenIndex();

//         // Every test index must have valid normalization parameters.
//         testIndex_->SetNormalizationParameters(
//             MakeTestNormalizationParameters()
//         );
//     }

//     void TearDown() override {
//         // Destroy the index before deleting its backing files.
//         testIndex_.reset();

//         std::filesystem::remove_all(tempDirectory_);
//     }

//     // Must remain a member of the friend class to access
//     // HalfHourIndex's private path-based constructor.
//     void OpenIndex() {
//         testIndex_.reset(
//             new Core::HalfHourIndex(indexPath_, metadataPath_)
//         );
//     }

//     void OpenIndexForWindow(
//         const std::filesystem::path& indexPath,
//         const std::filesystem::path& metadataPath,
//         const std::size_t inputWindowSeconds
//     ) {
//         testIndex_.reset(
//             new Core::HalfHourIndex(indexPath, metadataPath, inputWindowSeconds)
//         );
//     }

//     int IndexDimension() const {
//         return testIndex_->_index.d;
//     }

//     static std::filesystem::path CurrentIndexPath() {
//         return Core::HalfHourIndex::GetIndexPath(0);
//     }

//     static std::filesystem::path CurrentNormalizationPath() {
//         return Core::HalfHourIndex::GetNormalizationPath(0);
//     }

//     // Normalize a copy of the records before passing them to FAISS.
//     void AddRecords(
//         const std::vector<Core::MLRecord>& records,
//         const std::size_t recordOffset = 0,
//         const Core::NormalizationParameters& parameters =
//             MakeTestNormalizationParameters()
//     ) {
//         std::vector<Core::MLRecord> normalizedRecords = records;

//         std::span<Core::MLRecord> recordSpan(normalizedRecords);

//         Core::NormalizeRecordsVector(recordSpan, parameters);

//         testIndex_->AddVectorsToIndex(
//             recordSpan,
//             recordFixturePath_,
//             recordOffset
//         );
//     }

//     auto QueryIndex(
//         const std::vector<float>& query,
//         const std::size_t maxResults = 5
//     ) {
//         return testIndex_->Query(query, maxResults);
//     }

//     // Construct a normalized 60-dimensional query through HalfHourIndex.
//     // This also verifies that the index's normalization parameters are used.
//     std::vector<float> MakeQuery(
//         const std::vector<Core::MLRecord>& records,
//         const std::size_t firstRecord
//     ) const {
//         std::span<const Core::MLRecord> window(
//             records.data() + firstRecord,
//             WINDOW_LENGTH
//         );

//         return testIndex_->NormalizeQuery(window);
//     }

//     // Produce deterministic raw records for testing the indexing algorithm.
//     static std::vector<Core::MLRecord> MakeRecords(
//         const std::size_t count
//     ) {
//         std::vector<Core::MLRecord> records;
//         records.reserve(count);

//         for (std::size_t i = 0; i < count; ++i) {
//             const float value = static_cast<float>(i + 1);

//             Core::MLRecord record{};
//             record.mid_delta = value;
//             record.spread_delta = value * 0.25F;
//             record.volume = value * 0.5F;

//             records.push_back(record);
//         }

//         return records;
//     }
// };

// // ============================================================ //
// // FIXTURE TESTS
// // ============================================================ //

// // Verify that the immutable test fixtures are present.
// TEST_F(HalfHourIndex_Test, CheckedInFixturesExist) {
//     const auto corpusPath =
//         std::filesystem::path(SONNY_PROJECT_ROOT) /
//         "tests/test_storage/corpus/2026-09-01.corpus";

//     EXPECT_TRUE(std::filesystem::is_regular_file(corpusPath));
//     EXPECT_TRUE(std::filesystem::is_regular_file(recordFixturePath_));
// }

// TEST_F(HalfHourIndex_Test, InputWindowDimensionsMatchSupportedConfigurations) {
//     static_assert(Core::InputVectorDimension(30) == 30);
//     static_assert(Core::InputVectorDimension(45) == 45);
//     static_assert(Core::InputVectorDimension(60) == 60);

//     EXPECT_EQ(IndexDimension(), QUERY_DIMENSION);
//     EXPECT_EQ(CurrentIndexPath().parent_path().filename(),
//               std::format("{}s", Core::DEFAULT_INPUT_WINDOW_SECONDS));
//     EXPECT_EQ(CurrentNormalizationPath().parent_path().filename(),
//               std::format("{}s", Core::DEFAULT_INPUT_WINDOW_SECONDS));
// }

// TEST_F(HalfHourIndex_Test, BuildsAndQueriesEveryInputWindowConfiguration) {
//     for (const std::size_t inputWindowSeconds : Core::INPUT_WINDOW_CONFIGURATIONS) {
//         const std::filesystem::path windowDirectory =
//             tempDirectory_ / std::format("{}s", inputWindowSeconds);
//         std::filesystem::create_directories(windowDirectory);

//         const std::filesystem::path indexPath = windowDirectory / "test.index";
//         const std::filesystem::path metadataPath = windowDirectory / "test.meta";
//         const Core::NormalizationParameters parameters = MakeTestNormalizationParameters();
//         const std::vector<Core::MLRecord> rawRecords = MakeRecords(inputWindowSeconds);

//         OpenIndexForWindow(indexPath, metadataPath, inputWindowSeconds);
//         std::unique_ptr<Core::HalfHourIndex> index = std::move(testIndex_);
//         index->SetNormalizationParameters(parameters);

//         std::vector<Core::MLRecord> normalizedRecords = rawRecords;
//         std::span<Core::MLRecord> recordSpan(normalizedRecords);
//         Core::NormalizeRecordsVector(recordSpan, parameters);
//         index->AddVectorsToIndex(recordSpan, recordFixturePath_, 50);

//         const std::vector<float> query = index->NormalizeQuery(rawRecords);
//         ASSERT_EQ(query.size(), Core::InputVectorDimension(inputWindowSeconds));
//         const auto results = index->Query(query, 1);
//         ASSERT_EQ(results.size(), 1U);
//         EXPECT_NEAR(results.front().distance, 0.0F, 1e-6F);
//         EXPECT_EQ(results.front().recordIndex, 50U);

//         index.reset();

//         OpenIndexForWindow(indexPath, metadataPath, inputWindowSeconds);
//         std::unique_ptr<Core::HalfHourIndex> reloadedIndex = std::move(testIndex_);
//         EXPECT_EQ(reloadedIndex->NormalizeQuery(rawRecords), query);
//     }
// }

// TEST_F(HalfHourIndex_Test, EachQueryUsesTheMostRecentConfiguredRecords) {
//     const std::vector<Core::MLRecord> records = MakeRecords(Core::MAX_INPUT_WINDOW_SECONDS);
//     const std::span<const Core::MLRecord> recordSpan(records);

//     for (const std::size_t inputWindowSeconds : Core::INPUT_WINDOW_CONFIGURATIONS) {
//         const std::span<const Core::MLRecord> queryWindow =
//             Core::LatestInputWindow(recordSpan, inputWindowSeconds);

//         ASSERT_EQ(queryWindow.size(), inputWindowSeconds);
//         EXPECT_FLOAT_EQ(
//             queryWindow.front().mid_delta,
//             static_cast<float>(Core::MAX_INPUT_WINDOW_SECONDS - inputWindowSeconds + 1)
//         );
//         EXPECT_FLOAT_EQ(queryWindow.back().mid_delta,
//                         static_cast<float>(Core::MAX_INPUT_WINDOW_SECONDS));
//     }
// }

// TEST_F(HalfHourIndex_Test, InputQueueKeepsOnlyTheMostRecentConfiguredRecords) {
//     std::deque<int> records;

//     for (std::size_t i = 0; i < Core::MAX_INPUT_WINDOW_SECONDS + 2; ++i) {
//         Core::AppendInputWindowRecord(records, static_cast<int>(i));
//     }

//     ASSERT_EQ(records.size(), Core::MAX_INPUT_WINDOW_SECONDS);
//     EXPECT_EQ(records.front(), 2);
//     EXPECT_EQ(records.back(), static_cast<int>(Core::MAX_INPUT_WINDOW_SECONDS + 1));
// }

// TEST_F(HalfHourIndex_Test, HistoricalFutureBeginsAfterConfiguredInputWindow) {
//     EXPECT_EQ(Core::HistoricalFutureStart(100, WINDOW_LENGTH), 100 + WINDOW_LENGTH);
// }

// TEST_F(HalfHourIndex_Test, RejectsIndexWithAnotherInputWindowDimension) {
//     faiss::IndexFlatL2 incompatibleIndex(QUERY_DIMENSION - 2);
//     faiss::write_index(&incompatibleIndex, indexPath_.string().c_str());

//     EXPECT_THROW(OpenIndex(), std::runtime_error);
// }

// TEST_F(HalfHourIndex_Test, ReadsExistingRecordFixtureWithUnchangedRecordLayout) {
//     ASSERT_EQ(std::filesystem::file_size(recordFixturePath_) % sizeof(Core::MLRecord), 0U);

//     std::ifstream file(recordFixturePath_, std::ios::binary);
//     ASSERT_TRUE(file.is_open());

//     Core::MLRecord record{};
//     ASSERT_TRUE(file.read(reinterpret_cast<char*>(&record), sizeof(record)));

//     EXPECT_TRUE(std::isfinite(record.mid_delta));
//     EXPECT_TRUE(std::isfinite(record.spread_delta));
//     EXPECT_TRUE(std::isfinite(record.volume));
// }

// // ============================================================ //
// // QUERY TESTS
// // ============================================================ //

// // A new index should return no results.
// TEST_F(HalfHourIndex_Test, EmptyIndexReturnsNoResults) {
//     const std::vector<float> query(QUERY_DIMENSION, 0.0F);

//     const auto results = QueryIndex(query);

//     EXPECT_TRUE(results.empty());
// }

// // Queries must contain exactly the dimension configured for this index.
// TEST_F(HalfHourIndex_Test, QueryRejectsIncorrectDimension) {
//     const std::vector<float> query(QUERY_DIMENSION - 1, 0.0F);

//     EXPECT_THROW(
//         QueryIndex(query),
//         std::invalid_argument
//     );
// }

// // Query normalization requires exactly one configured input window.
// TEST_F(HalfHourIndex_Test, NormalizeQueryRejectsIncorrectRecordCount) {
//     const auto records = MakeRecords(WINDOW_LENGTH - 1);

//     std::span<const Core::MLRecord> recordSpan(records);

//     EXPECT_THROW(
//         testIndex_->NormalizeQuery(recordSpan),
//         std::invalid_argument
//     );
// }

// // Verify that normalization produces the expected feature ordering.
// TEST_F(HalfHourIndex_Test, NormalizeQueryProducesCorrectFeatureOrder) {
//     const auto records = MakeRecords(WINDOW_LENGTH);

//     const auto query = MakeQuery(records, 0);

//     ASSERT_EQ(query.size(), QUERY_DIMENSION);

//     // First record:
//     // mid_delta = 1.0
//     // spread_delta = 0.25
//     // Volume is preserved in MLRecord but excluded from the query vector.

//     EXPECT_FLOAT_EQ(query[0], 1.0F);
//     EXPECT_FLOAT_EQ(query[1], 2.0F);
// }

// // ============================================================ //
// // INDEXING TESTS
// // ============================================================ //

// // Fewer than the configured window length cannot produce a complete pattern.
// TEST_F(HalfHourIndex_Test, IgnoresInsufficientRecords) {
//     const auto records = MakeRecords(WINDOW_LENGTH - 1);

//     AddRecords(records);

//     const std::vector<float> query(QUERY_DIMENSION, 0.0F);
//     const auto results = QueryIndex(query);

//     EXPECT_TRUE(results.empty());
// }

// // Exactly one configured input window should produce exactly one vector.
// TEST_F(HalfHourIndex_Test, IndexesOneCompleteWindow) {
//     const auto records = MakeRecords(WINDOW_LENGTH);

//     AddRecords(records, 17);

//     const auto query = MakeQuery(records, 0);
//     const auto results = QueryIndex(query);

//     ASSERT_EQ(results.size(), 1U);

//     EXPECT_NEAR(results[0].distance, 0.0F, 1e-6F);
//     EXPECT_EQ(results[0].recordIndex, 17U);

//     const std::array<char, 8> expectedTradingDay{
//         '2', '0', '2', '6', '0', '9', '0', '1'
//     };

//     EXPECT_EQ(results[0].tradingDayId, expectedTradingDay);
// }

// // N records produce N - (window length - 1) overlapping windows.
// TEST_F(HalfHourIndex_Test, IndexesOverlappingWindows) {
//     const auto records = MakeRecords(WINDOW_LENGTH + 2);

//     AddRecords(records, 100);

//     const auto query = MakeQuery(records, 1);
//     const auto results = QueryIndex(query, 1);

//     ASSERT_EQ(results.size(), 1U);

//     // The matching window starts at record offset 100 + 1.
//     EXPECT_NEAR(results[0].distance, 0.0F, 1e-6F);
//     EXPECT_EQ(results[0].recordIndex, 101U);
// }

// // Query should respect the requested result limit.
// TEST_F(HalfHourIndex_Test, QueryRespectsMaxResults) {
//     const auto records = MakeRecords(WINDOW_LENGTH + 5);

//     AddRecords(records);

//     const auto query = MakeQuery(records, 0);

//     EXPECT_EQ(QueryIndex(query, 2).size(), 2U);
//     EXPECT_TRUE(QueryIndex(query, 0).empty());
// }

// // ============================================================ //
// // NORMALIZATION PERSISTENCE TESTS
// // ============================================================ //

// // Index vectors, metadata, and normalization parameters
// // should survive a complete disk round trip.
// TEST_F(HalfHourIndex_Test, PersistsIndexMetadataAndNormalization) {
//     auto parameters = MakeTestNormalizationParameters();

//     // Use distinctive parameters so we can verify that the exact
//     // normalization configuration survives JSON serialization.
//     parameters.mid_delta_mean = 2.5;
//     parameters.mid_delta_stddev = 3.5;

//     testIndex_->SetNormalizationParameters(parameters);

//     const auto records = MakeRecords(31);

//     AddRecords(records, 300, parameters);

//     // Query the second overlapping window.
//     const auto queryBeforeSave = MakeQuery(records, 1);

//     // Destroying an updated index saves the index, metadata, and normalization JSON.
//     testIndex_.reset();

//     ASSERT_TRUE(std::filesystem::is_regular_file(indexPath_));
//     ASSERT_TRUE(std::filesystem::is_regular_file(metadataPath_));
//     ASSERT_TRUE(std::filesystem::is_regular_file(normalizationPath_));

//     std::ifstream normalizationFile(normalizationPath_);
//     const std::string normalizationJson(
//         (std::istreambuf_iterator<char>(normalizationFile)),
//         std::istreambuf_iterator<char>()
//     );
//     EXPECT_NE(
//         normalizationJson.find(std::format(
//             "\"input_window_seconds\":{}", Core::DEFAULT_INPUT_WINDOW_SECONDS)),
//         std::string::npos
//     );

//     // Reopen the same files using the test-only constructor.
//     // This should load the normalization parameters from JSON.
//     OpenIndex();

//     const auto queryAfterLoad = MakeQuery(records, 1);

//     ASSERT_EQ(queryAfterLoad.size(), QUERY_DIMENSION);

//     // Verify that the loaded normalization parameters produce
//     // exactly the same query as the original parameters.
//     EXPECT_EQ(queryAfterLoad, queryBeforeSave);

//     const auto results = QueryIndex(queryAfterLoad, 2);

//     ASSERT_EQ(results.size(), 2U);

//     EXPECT_NEAR(results[0].distance, 0.0F, 1e-6F);
//     EXPECT_EQ(results[0].recordIndex, 301U);

//     const std::array<char, 8> expectedTradingDay{
//         '2', '0', '2', '6', '0', '9', '0', '1'
//     };

//     EXPECT_EQ(results[0].tradingDayId, expectedTradingDay);
// }

// // An existing index without its normalization JSON must not be usable.
// TEST_F(HalfHourIndex_Test, RejectsIndexWithoutNormalizationParameters) {
//     // Save the initial empty index and its normalization parameters.
//     testIndex_.reset();

//     ASSERT_TRUE(std::filesystem::is_regular_file(indexPath_));
//     ASSERT_TRUE(std::filesystem::is_regular_file(normalizationPath_));

//     // Simulate a legacy index or missing normalization file.
//     std::filesystem::remove(normalizationPath_);

//     EXPECT_THROW(
//         OpenIndex(),
//         std::runtime_error
//     );
// }

// TEST_F(HalfHourIndex_Test, RejectsNormalizationFromAnotherInputWindow) {
//     testIndex_.reset();

//     std::ifstream input(normalizationPath_);
//     std::string normalizationJson(
//         (std::istreambuf_iterator<char>(input)),
//         std::istreambuf_iterator<char>()
//     );
//     const std::string configuredWindow = std::format(
//         "\"input_window_seconds\":{}", Core::DEFAULT_INPUT_WINDOW_SECONDS
//     );
//     const std::size_t configuredWindowPosition = normalizationJson.find(configuredWindow);
//     ASSERT_NE(configuredWindowPosition, std::string::npos);

//     const std::size_t incompatibleWindow = 10;
//     normalizationJson.replace(
//         configuredWindowPosition,
//         configuredWindow.size(),
//         std::format("\"input_window_seconds\":{}", incompatibleWindow)
//     );

//     std::ofstream output(normalizationPath_, std::ios::trunc);
//     ASSERT_TRUE(output.is_open());
//     output << normalizationJson;
//     output.close();

//     EXPECT_THROW(OpenIndex(), std::runtime_error);
// }
