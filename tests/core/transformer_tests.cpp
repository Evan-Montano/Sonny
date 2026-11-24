// transformer_tests.cpp
// Tests for normalization statistics, record normalization, and Webull quote conversion.

#include <gtest/gtest.h>

#include "core/inputconfig.hpp"
#include "core/transformer.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

// Record files are raw arrays of MLRecord: one float MidDelta per second.
static_assert(sizeof(Core::MLRecord) == 4);
static_assert(offsetof(Core::MLRecord, MidDelta) == 0);

namespace {

    Client::WebullQuote MakeQuote(const double bid, const double ask) {
        Client::WebullQuote quote{};
        quote.Bids.push_back({.Price = bid, .Size = 100});
        quote.Asks.push_back({.Price = ask, .Size = 100});
        return quote;
    }

}

// ========== NORMALIZATION ACCUMULATOR ========== //

TEST(NormalizationTests, CalculatesPooledPopulationStatistics) {
    Core::NormalizationAccumulator accumulator{};

    const std::vector<Core::MLRecord> records{{1.0f}, {3.0f}, {5.0f}};
    accumulator.AddRecords(records);

    const auto parameters = accumulator.GetParameters();

    EXPECT_EQ(parameters.SampleCount, 3u);
    EXPECT_DOUBLE_EQ(parameters.MidDeltaMean, 3.0);
    EXPECT_NEAR(parameters.MidDeltaStddev, std::sqrt(8.0 / 3.0), 1e-10);
}

TEST(NormalizationTests, PoolsStatisticsAcrossSeparateBatches) {
    Core::NormalizationAccumulator pooled{};
    pooled.AddRecords(std::vector<Core::MLRecord>{{1.0f}, {3.0f}});
    pooled.AddRecords(std::vector<Core::MLRecord>{{5.0f}});

    Core::NormalizationAccumulator single{};
    single.AddRecords(std::vector<Core::MLRecord>{{1.0f}, {3.0f}, {5.0f}});

    EXPECT_DOUBLE_EQ(pooled.GetParameters().MidDeltaMean, single.GetParameters().MidDeltaMean);
    EXPECT_DOUBLE_EQ(pooled.GetParameters().MidDeltaStddev, single.GetParameters().MidDeltaStddev);
}

TEST(NormalizationTests, RejectsEmptyAndNonFiniteInput) {
    Core::NormalizationAccumulator accumulator{};

    EXPECT_THROW(accumulator.GetParameters(), std::runtime_error);
    EXPECT_THROW(accumulator.AddRecord({std::numeric_limits<float>::quiet_NaN()}), std::invalid_argument);
    EXPECT_THROW(accumulator.AddRecord({std::numeric_limits<float>::infinity()}), std::invalid_argument);
}

// ========== VALIDATION ========== //

TEST(NormalizationTests, ValidatesParameters) {
    EXPECT_NO_THROW(Core::ValidateNormalizationParameters({.SampleCount = 1, .MidDeltaMean = 0.0, .MidDeltaStddev = 0.0}));

    EXPECT_THROW(
        Core::ValidateNormalizationParameters({.SampleCount = 0, .MidDeltaMean = 0.0, .MidDeltaStddev = 1.0}),
        std::invalid_argument
    );
    EXPECT_THROW(
        Core::ValidateNormalizationParameters({.SampleCount = 1, .MidDeltaMean = 0.0, .MidDeltaStddev = -1.0}),
        std::invalid_argument
    );
    EXPECT_THROW(
        Core::ValidateNormalizationParameters({
            .SampleCount = 1,
            .MidDeltaMean = std::numeric_limits<double>::quiet_NaN(),
            .MidDeltaStddev = 1.0
        }),
        std::invalid_argument
    );
}

// ========== NORMALIZATION ========== //

TEST(NormalizationTests, FlattensConfiguredWindowIntoMidDeltaFeatures) {
    const Core::NormalizationParameters parameters{
        .SampleCount = 100,
        .MidDeltaMean = 1.0,
        .MidDeltaStddev = 2.0
    };

    const std::vector<Core::MLRecord> records(Core::DEFAULT_INPUT_WINDOW_SECONDS, Core::MLRecord{5.0f});

    const std::vector<float> query = Core::NormalizeRecordsToVector(records, parameters);

    ASSERT_EQ(query.size(), Core::InputVectorDimension(Core::DEFAULT_INPUT_WINDOW_SECONDS));

    // (5 - 1) / 2 = 2 for every second; one feature per record.
    for (const float value : query) {
        EXPECT_FLOAT_EQ(value, 2.0f);
    }
}

TEST(NormalizationTests, NormalizesInPlaceAndMatchesTheFlattenedVector) {
    const Core::NormalizationParameters parameters{
        .SampleCount = 100,
        .MidDeltaMean = 0.5,
        .MidDeltaStddev = 0.25
    };

    std::vector<Core::MLRecord> records{{0.0f}, {0.5f}, {1.0f}};
    const std::vector<float> flattened = Core::NormalizeRecordsToVector(records, parameters);

    Core::NormalizeRecordsVector(records, parameters);

    ASSERT_EQ(flattened.size(), records.size());
    EXPECT_FLOAT_EQ(records[0].MidDelta, -2.0f);
    EXPECT_FLOAT_EQ(records[1].MidDelta, 0.0f);
    EXPECT_FLOAT_EQ(records[2].MidDelta, 2.0f);

    for (std::size_t i = 0; i < records.size(); ++i) {
        EXPECT_FLOAT_EQ(flattened[i], records[i].MidDelta);
    }
}

TEST(NormalizationTests, HandlesZeroStandardDeviation) {
    const Core::NormalizationParameters parameters{
        .SampleCount = 100,
        .MidDeltaMean = 5.0,
        .MidDeltaStddev = 0.0
    };

    std::vector<Core::MLRecord> records{{10.0f}};
    Core::NormalizeRecordsVector(records, parameters);

    EXPECT_FLOAT_EQ(records[0].MidDelta, 0.0f);
}

TEST(NormalizationTests, RejectsNonFiniteRecords) {
    const Core::NormalizationParameters parameters{
        .SampleCount = 100,
        .MidDeltaMean = 0.0,
        .MidDeltaStddev = 1.0
    };

    const std::vector<Core::MLRecord> records{{std::numeric_limits<float>::quiet_NaN()}};

    EXPECT_THROW(Core::NormalizeRecordsToVector(records, parameters), std::invalid_argument);
}

// ========== WEBULL QUOTE CONVERSION ========== //

TEST(WebullQuoteConversionTests, ComputesTheMidpointDelta) {
    const auto record = Core::WebullQuoteToMLRecord(MakeQuote(99.98, 100.02), MakeQuote(100.03, 100.05));

    ASSERT_TRUE(record.has_value());
    EXPECT_NEAR(record->MidDelta, 0.04f, 1e-5f);
}

TEST(WebullQuoteConversionTests, RejectsQuotesWithAnEmptySide) {
    Client::WebullQuote oneSided = MakeQuote(100.0, 100.02);
    oneSided.Asks.clear();

    EXPECT_FALSE(Core::WebullQuoteToMLRecord(oneSided, MakeQuote(100.0, 100.02)).has_value());
    EXPECT_FALSE(Core::WebullQuoteToMLRecord(MakeQuote(100.0, 100.02), oneSided).has_value());
}
