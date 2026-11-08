// #include <gtest/gtest.h>

// #include "core/transformer.hpp"
// #include "core/inputconfig.hpp"

// #include <cmath>
// #include <cstddef>
// #include <vector>

// static_assert(sizeof(Core::MLRecord) == 12);
// static_assert(offsetof(Core::MLRecord, mid_delta) == 0);
// static_assert(offsetof(Core::MLRecord, spread_delta) == 4);
// static_assert(offsetof(Core::MLRecord, volume) == 8);

// TEST(NormalizationTests, CalculatesPooledPopulationStatistics) {
//     Core::NormalizationAccumulator accumulator{};

//     const std::vector<Core::MLRecord> records{
//         {1.0f, 2.0f, 0.0f},
//         {3.0f, 4.0f, 3.0f},
//         {5.0f, 6.0f, 8.0f}
//     };

//     accumulator.AddRecords(records);

//     const auto parameters = accumulator.GetParameters();

//     EXPECT_EQ(parameters.sample_count, 3);

//     EXPECT_DOUBLE_EQ(parameters.mid_delta_mean, 3.0);

//     EXPECT_NEAR(
//         parameters.mid_delta_stddev,
//         std::sqrt(8.0 / 3.0),
//         1e-10
//     );

// }

// TEST(NormalizationTests, FlattensConfiguredWindowIntoPriceFeatures) {
//     Core::NormalizationParameters parameters{
//         .sample_count = 100,

//         .mid_delta_mean = 0.0,
//         .mid_delta_stddev = 1.0
//     };

//     const std::vector<Core::MLRecord> records(
//         Core::DEFAULT_INPUT_WINDOW_SECONDS,
//         Core::MLRecord{1.0f, 2.0f, 3.0f}
//     );

//     const std::vector<float> query =
//         Core::NormalizeRecordsToVector(records, parameters);

//     ASSERT_EQ(query.size(), Core::InputVectorDimension(Core::DEFAULT_INPUT_WINDOW_SECONDS));

//     EXPECT_FLOAT_EQ(query[0], 1.0f);
//     EXPECT_FLOAT_EQ(query[1], 2.0f);
//     EXPECT_FLOAT_EQ(query[2], 1.0f);
//     EXPECT_FLOAT_EQ(query[3], 2.0f);
// }

// TEST(NormalizationTests, HandlesZeroStandardDeviation) {
//     Core::NormalizationParameters parameters{
//         .sample_count = 100,

//         .mid_delta_mean = 5.0,
//         .mid_delta_stddev = 0.0
//     };

//     std::vector<Core::MLRecord> records{
//         {10.0f, 20.0f, 30.0f}
//     };

//     Core::NormalizeRecordsVector(records, parameters);

//     EXPECT_FLOAT_EQ(records[0].mid_delta, 0.0f);
//     EXPECT_FLOAT_EQ(records[0].spread_delta, 0.0f);
//     EXPECT_FLOAT_EQ(records[0].volume, 30.0f);
// }

// TEST(NormalizationTests, IgnoresVolumeDuringStatisticsAndVectorConstruction) {
//     Core::NormalizationAccumulator accumulator{};

//     accumulator.AddRecord({1.0f, 2.0f, -1.0f});
//     const auto parameters = accumulator.GetParameters();

//     const std::vector<Core::MLRecord> records{
//         {1.0f, 2.0f, -1.0f},
//         {1.0f, 2.0f, 9000.0f}
//     };
//     const auto vector = Core::NormalizeRecordsToVector(records, parameters);

//     EXPECT_EQ(vector, (std::vector<float>{0.0f, 0.0f, 0.0f, 0.0f}));
// }
