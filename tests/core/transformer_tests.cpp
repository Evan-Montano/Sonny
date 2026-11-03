#include <gtest/gtest.h>

#include "core/transformer.hpp"

#include <cmath>
#include <vector>

TEST(NormalizationTests, CalculatesPooledPopulationStatistics) {
    Core::NormalizationAccumulator accumulator{};

    const std::vector<Core::MLRecord> records{
        {1.0f, 2.0f, 0.0f},
        {3.0f, 4.0f, 3.0f},
        {5.0f, 6.0f, 8.0f}
    };

    accumulator.AddRecords(records);

    const auto parameters = accumulator.GetParameters();

    EXPECT_EQ(parameters.sample_count, 3);

    EXPECT_DOUBLE_EQ(parameters.mid_delta_mean, 3.0);
    EXPECT_DOUBLE_EQ(parameters.spread_delta_mean, 4.0);

    EXPECT_NEAR(
        parameters.mid_delta_stddev,
        std::sqrt(8.0 / 3.0),
        1e-10
    );

    const double logVolumeMean =
        (std::log1p(0.0) +
         std::log1p(3.0) +
         std::log1p(8.0)) / 3.0;

    EXPECT_NEAR(
        parameters.volume_mean,
        logVolumeMean,
        1e-10
    );
}

TEST(NormalizationTests, FlattensThirtyRecordsIntoNinetyFeatures) {
    Core::NormalizationParameters parameters{
        .sample_count = 100,

        .mid_delta_mean = 0.0,
        .mid_delta_stddev = 1.0,

        .spread_delta_mean = 0.0,
        .spread_delta_stddev = 1.0,

        .volume_mean = 0.0,
        .volume_stddev = 1.0
    };

    const std::vector<Core::MLRecord> records(
        30,
        Core::MLRecord{1.0f, 2.0f, 3.0f}
    );

    const std::vector<float> query =
        Core::NormalizeRecordsToVector(records, parameters);

    ASSERT_EQ(query.size(), 90);

    EXPECT_FLOAT_EQ(query[0], 1.0f);
    EXPECT_FLOAT_EQ(query[1], 2.0f);
    EXPECT_FLOAT_EQ(query[2], std::log1p(3.0f));
}

TEST(NormalizationTests, HandlesZeroStandardDeviation) {
    Core::NormalizationParameters parameters{
        .sample_count = 100,

        .mid_delta_mean = 5.0,
        .mid_delta_stddev = 0.0,

        .spread_delta_mean = 2.0,
        .spread_delta_stddev = 0.0,

        .volume_mean = 1.0,
        .volume_stddev = 0.0
    };

    std::vector<Core::MLRecord> records{
        {10.0f, 20.0f, 30.0f}
    };

    Core::NormalizeRecordsVector(records, parameters);

    EXPECT_FLOAT_EQ(records[0].mid_delta, 0.0f);
    EXPECT_FLOAT_EQ(records[0].spread_delta, 0.0f);
    EXPECT_FLOAT_EQ(records[0].volume, 0.0f);
}

TEST(NormalizationTests, RejectsNegativeVolume) {
    Core::NormalizationAccumulator accumulator{};

    EXPECT_THROW(
        accumulator.AddRecord({1.0f, 2.0f, -1.0f}),
        std::invalid_argument
    );
}