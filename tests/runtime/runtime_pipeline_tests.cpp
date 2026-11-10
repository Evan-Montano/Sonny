// runtime_pipeline_tests.cpp
// Tests for the live pipeline stages: session clock, one-second aggregation, rolling window, historical futures, and forecasting.

#include "core/inputconfig.hpp"
#include "runtime/forecast.hpp"
#include "runtime/historicalrecordstore.hpp"
#include "runtime/liveinputwindow.hpp"
#include "runtime/marketclock.hpp"
#include "runtime/onesecondaggregator.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

    // 2026-11-05 09:30:00 ET (EST, UTC-5), a Thursday.
    constexpr std::int64_t THURSDAY_OPEN_MS = 1793889000LL * 1000;

    // 2026-07-06 09:30:00 ET (EDT, UTC-4), a Monday.
    constexpr std::int64_t SUMMER_MONDAY_OPEN_MS = 1783344600LL * 1000;

    Client::WebullQuote MakeQuote(const std::int64_t timestampMs, const double bid, const double ask) {
        Client::WebullQuote quote{};
        quote.symbol = "SPY";
        quote.timestamp_ms = timestampMs;
        quote.bids.push_back({.Price = bid, .size = 100});
        quote.asks.push_back({.Price = ask, .size = 100});
        return quote;
    }

    /**
     * @brief Collects everything an aggregator emits.
     */
    struct AggregatorHarness {
        std::vector<Runtime::LiveRecord> records;
        int resets = 0;

        Runtime::OneSecondAggregator aggregator{
            [this](const Runtime::LiveRecord& record) { records.push_back(record); },
            [this] { ++resets; }
        };

        // Sends one quote at the start of each listed second (relative to the open) with the given midpoint.
        void Send(const std::int64_t second, const double mid, const std::int64_t offsetMs = 0) {
            aggregator.AddQuote(MakeQuote(THURSDAY_OPEN_MS + second * 1000 + offsetMs, mid - 0.01, mid + 0.01));
        }
    };

    Runtime::LiveRecord MakeLiveRecord(const std::int64_t second, const float midDelta = 0.0f) {
        return Runtime::LiveRecord{
            .record = Core::MLRecord{.mid_delta = midDelta},
            .timestamp_ms = second * 1000,
            .observed = true
        };
    }

}

// ========== MARKET CLOCK ========== //

TEST(MarketClockTests, CountsSecondsFromTheEasternOpenInWinterAndSummer) {
    EXPECT_EQ(Runtime::GetSecondsSinceMarketOpen(THURSDAY_OPEN_MS), 0u);
    EXPECT_EQ(Runtime::GetSecondsSinceMarketOpen(THURSDAY_OPEN_MS + 90'500), 90u);
    EXPECT_EQ(Runtime::GetSecondsSinceMarketOpen(SUMMER_MONDAY_OPEN_MS + 1'000), 1u);
}

TEST(MarketClockTests, RejectsPreMarketAndWeekends) {
    EXPECT_FALSE(Runtime::GetSecondsSinceMarketOpen(THURSDAY_OPEN_MS - 1).has_value());

    constexpr std::int64_t SATURDAY_OPEN_MS = THURSDAY_OPEN_MS + 2LL * 24 * 60 * 60 * 1000;
    EXPECT_FALSE(Runtime::GetSecondsSinceMarketOpen(SATURDAY_OPEN_MS).has_value());
}

TEST(MarketClockTests, ClampsAfterHoursToTheLastSessionSecond) {
    const std::int64_t closeMs = THURSDAY_OPEN_MS + static_cast<std::int64_t>(Runtime::SESSION_SECONDS) * 1000;
    EXPECT_EQ(Runtime::GetSecondsSinceMarketOpen(closeMs), Runtime::SESSION_SECONDS - 1);
}

TEST(MarketClockTests, WindowStartRejectsWindowsThatReachIntoPreMarket) {
    EXPECT_FALSE(Runtime::GetWindowStartSecond(8, 10).has_value());
    EXPECT_EQ(Runtime::GetWindowStartSecond(9, 10), 0u);
    EXPECT_EQ(Runtime::GetWindowStartSecond(1829, 30), 1800u);
}

// ========== ONE SECOND AGGREGATOR ========== //

TEST(OneSecondAggregatorTests, EmitsOneRecordPerSecondFromEachClosingQuote) {
    AggregatorHarness harness;

    harness.Send(0, 100.00);          // Baseline second.
    harness.Send(1, 100.50);
    harness.Send(1, 100.10, 900);     // Closing quote of second 1.
    harness.Send(2, 100.20);          // Completes second 1.

    ASSERT_EQ(harness.records.size(), 1u);
    EXPECT_NEAR(harness.records[0].record.mid_delta, 0.10, 1e-4);
    EXPECT_EQ(harness.records[0].timestamp_ms / 1000, THURSDAY_OPEN_MS / 1000 + 1);
    EXPECT_TRUE(harness.records[0].observed);
}

TEST(OneSecondAggregatorTests, QuietSecondsCarryThePriceForwardWithZeroDeltas) {
    AggregatorHarness harness;

    harness.Send(0, 100.00);
    harness.Send(1, 100.10);
    harness.Send(4, 100.40);          // Seconds 2 and 3 are quiet.
    harness.Send(5, 100.40);          // Completes second 4.

    ASSERT_EQ(harness.records.size(), 4u);

    double cumulative = 0.0;
    for (std::size_t i = 0; i < harness.records.size(); ++i) {
        EXPECT_EQ(harness.records[i].timestamp_ms / 1000, THURSDAY_OPEN_MS / 1000 + 1 + static_cast<std::int64_t>(i));
        cumulative += harness.records[i].record.mid_delta;
    }

    EXPECT_FALSE(harness.records[1].observed);
    EXPECT_FALSE(harness.records[2].observed);
    EXPECT_FLOAT_EQ(harness.records[1].record.mid_delta, 0.0f);
    EXPECT_FLOAT_EQ(harness.records[2].record.mid_delta, 0.0f);

    // The full move is attributed to second 4, so the cumulative path matches the real price.
    EXPECT_NEAR(harness.records[3].record.mid_delta, 0.30, 1e-4);
    EXPECT_NEAR(cumulative, 0.40, 1e-4);
    EXPECT_EQ(harness.resets, 0);
}

TEST(OneSecondAggregatorTests, LongInterruptionsResetInsteadOfFillingTheGap) {
    AggregatorHarness harness;

    harness.Send(0, 100.00);
    harness.Send(1, 100.10);
    harness.Send(2, 100.20);
    const std::size_t recordsBeforeGap = harness.records.size();

    harness.Send(2 + Runtime::OneSecondAggregator::MAX_CARRIED_FORWARD_SECONDS + 2, 101.00);

    EXPECT_EQ(harness.resets, 1);
    EXPECT_EQ(harness.records.size(), recordsBeforeGap);
}

TEST(OneSecondAggregatorTests, OneSidedBookResetsContinuity) {
    AggregatorHarness harness;

    harness.Send(0, 100.00);
    harness.Send(1, 100.10);

    Client::WebullQuote oneSided = MakeQuote(THURSDAY_OPEN_MS + 2000, 100.0, 100.2);
    oneSided.asks.clear();
    harness.aggregator.AddQuote(oneSided);

    harness.Send(3, 100.30);          // Completes the one-sided second 2.
    EXPECT_EQ(harness.resets, 1);

    // Second 3 becomes the new baseline, so the next record is second 4's move from it.
    harness.Send(4, 100.35);
    harness.Send(5, 100.35);
    ASSERT_FALSE(harness.records.empty());
    EXPECT_NEAR(harness.records.back().record.mid_delta, 0.05, 1e-4);
}

TEST(OneSecondAggregatorTests, IgnoresQuotesFromAnEarlierSecond) {
    AggregatorHarness harness;

    harness.Send(0, 100.00);
    harness.Send(1, 100.10);
    harness.Send(0, 99.00, 500);      // Late, already finalized.
    harness.Send(2, 100.10);

    ASSERT_EQ(harness.records.size(), 1u);
    EXPECT_NEAR(harness.records[0].record.mid_delta, 0.10, 1e-4);
}

// ========== LIVE INPUT WINDOW ========== //

TEST(LiveInputWindowTests, SnapshotsOnlyFullUnchangedWindowsOnce) {
    Runtime::LiveInputWindow window;

    for (std::int64_t second = 0; second < static_cast<std::int64_t>(Core::MAX_INPUT_WINDOW_SECONDS) - 1; ++second) {
        window.Push(MakeLiveRecord(second));
    }
    EXPECT_FALSE(window.TakeSnapshotIfUpdated().has_value());

    window.Push(MakeLiveRecord(static_cast<std::int64_t>(Core::MAX_INPUT_WINDOW_SECONDS) - 1, 0.5f));

    const std::optional<Runtime::LiveInputWindow::Snapshot> snapshot = window.TakeSnapshotIfUpdated();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->records.size(), Core::MAX_INPUT_WINDOW_SECONDS);
    EXPECT_FLOAT_EQ(snapshot->records.back().mid_delta, 0.5f);
    EXPECT_EQ(snapshot->last_timestamp_ms, (static_cast<std::int64_t>(Core::MAX_INPUT_WINDOW_SECONDS) - 1) * 1000);

    EXPECT_FALSE(window.TakeSnapshotIfUpdated().has_value());
}

TEST(LiveInputWindowTests, NonConsecutiveRecordClearsTheWindow) {
    Runtime::LiveInputWindow window;

    window.Push(MakeLiveRecord(10));
    window.Push(MakeLiveRecord(11));

    EXPECT_EQ(window.Push(MakeLiveRecord(13)), 1u);
}

// ========== HISTORICAL RECORD STORE ========== //

/**
 * @brief Runs inside a temporary working directory, since the store reads relative storage/records/ paths.
 */
class HistoricalRecordStoreTests : public ::testing::Test {
protected:
    std::filesystem::path previousWorkingDirectory_;
    std::filesystem::path tempDirectory_;

    void SetUp() override {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        tempDirectory_ = std::filesystem::temp_directory_path() / ("sonny_record_store_tests_" + std::to_string(timestamp));
        std::filesystem::create_directories(tempDirectory_ / "storage/records");

        previousWorkingDirectory_ = std::filesystem::current_path();
        std::filesystem::current_path(tempDirectory_);
    }

    void TearDown() override {
        std::filesystem::current_path(previousWorkingDirectory_);
        std::filesystem::remove_all(tempDirectory_);
    }

    static void WriteBytes(const std::string& fileName, const char* data, const std::size_t size) {
        std::ofstream file(std::filesystem::path("storage/records") / fileName, std::ios::binary);
        file.write(data, static_cast<std::streamsize>(size));
    }
};

TEST_F(HistoricalRecordStoreTests, LoadsADayByItsMetadataId) {
    const std::vector<Core::MLRecord> records{{0.0f}, {0.01f}, {-0.02f}};
    WriteBytes("2026-09-01.rec", reinterpret_cast<const char*>(records.data()), records.size() * sizeof(Core::MLRecord));

    Runtime::HistoricalRecordStore store;
    const std::vector<Core::MLRecord>* day = store.Find({'2', '0', '2', '6', '0', '9', '0', '1'});

    ASSERT_NE(day, nullptr);
    ASSERT_EQ(day->size(), 3u);
    EXPECT_FLOAT_EQ((*day)[1].mid_delta, 0.01f);
    EXPECT_FLOAT_EQ((*day)[2].mid_delta, -0.02f);

    // A second lookup is served from the cache.
    EXPECT_EQ(store.Find({'2', '0', '2', '6', '0', '9', '0', '1'}), day);
}

TEST_F(HistoricalRecordStoreTests, RejectsMissingAndTruncatedDays) {
    // 6 bytes is not a whole number of 4-byte records.
    const char bytes[6]{};
    WriteBytes("2026-09-02.rec", bytes, sizeof(bytes));

    Runtime::HistoricalRecordStore store;

    EXPECT_EQ(store.Find({'2', '0', '2', '6', '0', '9', '0', '3'}), nullptr);
    EXPECT_EQ(store.Find({'2', '0', '2', '6', '0', '9', '0', '2'}), nullptr);
}

// ========== FORECAST ========== //

TEST(ForecastTests, MagnitudeDoesNotCancelWhenMatchesDisagree) {
    // Two magnitude matches moving equally far in opposite directions: the old signed average was 0.
    Runtime::HistoricalMatch up{.weight = 3.0};
    Runtime::HistoricalMatch down{.weight = 1.0};
    up.mid_deltas.fill(0.01);
    down.mid_deltas.fill(-0.01);

    // Direction comes only from the direction matches, which all move up.
    Runtime::HistoricalMatch directionUp{.weight = 1.0};
    directionUp.mid_deltas.fill(0.02);

    const std::vector<Runtime::HistoricalMatch> magnitudeMatches{up, down};
    const std::vector<Runtime::HistoricalMatch> directionMatches{directionUp, directionUp};
    const Runtime::ForecastSummary summary = Runtime::BuildForecast(magnitudeMatches, directionMatches, 1234);

    EXPECT_EQ(summary.forecast.as_of_timestamp_ms, 1234);
    EXPECT_EQ(summary.forecast.historical_matches, 2u);
    EXPECT_EQ(summary.magnitude_matches, 2u);
    EXPECT_EQ(summary.direction_matches, 2u);
    EXPECT_NEAR(summary.magnitude_effective_matches, 16.0 / 10.0, 1e-9);
    EXPECT_NEAR(summary.direction_effective_matches, 2.0, 1e-9);

    for (std::size_t i = 0; i < Runtime::PredictionEvaluationHorizons.size(); ++i) {
        const double horizon = static_cast<double>(Runtime::PredictionEvaluationHorizons[i]);

        // Both magnitude paths move 0.01h in absolute terms, so the expected magnitude is 0.01h,
        // and the direction matches unanimously vote up.
        EXPECT_NEAR(summary.expected_magnitudes[i], 0.01 * horizon, 1e-9);
        EXPECT_NEAR(summary.direction_agreement[i], 1.0, 1e-9);
        EXPECT_NEAR(summary.forecast.cumulative_changes[i], 0.01 * horizon, 1e-9);
    }
}

TEST(ForecastTests, DirectionFollowsTheWeightedVote) {
    Runtime::HistoricalMatch magnitude{.weight = 1.0};
    magnitude.mid_deltas.fill(0.01);

    Runtime::HistoricalMatch down{.weight = 3.0};
    Runtime::HistoricalMatch up{.weight = 1.0};
    down.mid_deltas.fill(-0.01);
    up.mid_deltas.fill(0.05);

    const std::vector<Runtime::HistoricalMatch> magnitudeMatches{magnitude};
    const std::vector<Runtime::HistoricalMatch> directionMatches{down, up};
    const Runtime::ForecastSummary summary = Runtime::BuildForecast(magnitudeMatches, directionMatches, 0);

    for (std::size_t i = 0; i < Runtime::PredictionEvaluationHorizons.size(); ++i) {
        const double horizon = static_cast<double>(Runtime::PredictionEvaluationHorizons[i]);

        // Votes are signs, not sizes: (3 × −1 + 1 × +1) / 4 = −0.5, despite "up" moving further.
        EXPECT_NEAR(summary.direction_agreement[i], -0.5, 1e-9);
        EXPECT_NEAR(summary.forecast.cumulative_changes[i], -0.01 * horizon, 1e-9);
    }
}

TEST(ForecastTests, SplitDirectionVoteForecastsNoChange) {
    Runtime::HistoricalMatch magnitude{.weight = 1.0};
    magnitude.mid_deltas.fill(0.01);

    Runtime::HistoricalMatch up{.weight = 1.0};
    Runtime::HistoricalMatch down{.weight = 1.0};
    up.mid_deltas.fill(0.01);
    down.mid_deltas.fill(-0.01);

    const std::vector<Runtime::HistoricalMatch> magnitudeMatches{magnitude};
    const std::vector<Runtime::HistoricalMatch> directionMatches{up, down};
    const Runtime::ForecastSummary summary = Runtime::BuildForecast(magnitudeMatches, directionMatches, 0);

    for (std::size_t i = 0; i < Runtime::PredictionEvaluationHorizons.size(); ++i) {
        EXPECT_DOUBLE_EQ(summary.direction_agreement[i], 0.0);
        EXPECT_DOUBLE_EQ(summary.forecast.cumulative_changes[i], 0.0);
        EXPECT_GT(summary.expected_magnitudes[i], 0.0);
    }
}

TEST(ForecastTests, FlatDirectionMatchesDoNotVote) {
    Runtime::HistoricalMatch magnitude{.weight = 1.0};
    magnitude.mid_deltas.fill(0.01);

    // Cumulative moves below FLAT_MOVE_EPSILON count as flat for every horizon.
    Runtime::HistoricalMatch flat{.weight = 10.0};
    flat.mid_deltas.fill(Runtime::FLAT_MOVE_EPSILON / 100.0);
    Runtime::HistoricalMatch up{.weight = 1.0};
    up.mid_deltas.fill(0.01);

    const std::vector<Runtime::HistoricalMatch> magnitudeMatches{magnitude};
    const std::vector<Runtime::HistoricalMatch> directionMatches{flat, up};
    const Runtime::ForecastSummary summary = Runtime::BuildForecast(magnitudeMatches, directionMatches, 0);

    for (std::size_t i = 0; i < Runtime::PredictionEvaluationHorizons.size(); ++i) {
        EXPECT_NEAR(summary.direction_agreement[i], 1.0 / 11.0, 1e-9);
        EXPECT_GT(summary.forecast.cumulative_changes[i], 0.0);
    }
}

TEST(ForecastTests, CloserMatchesWeighMore) {
    EXPECT_GT(Runtime::MatchWeight(0.5f), Runtime::MatchWeight(1.0f));
    EXPECT_TRUE(std::isfinite(Runtime::MatchWeight(0.0f)));

    EXPECT_GT(Runtime::DirectionMatchWeight(0.0f), Runtime::DirectionMatchWeight(1.0f));
    EXPECT_DOUBLE_EQ(Runtime::DirectionMatchWeight(0.0f), 1.0);
}

TEST(ForecastTests, RejectsEmptyMatches) {
    Runtime::HistoricalMatch match{.weight = 1.0};
    const std::vector<Runtime::HistoricalMatch> matches{match};

    EXPECT_THROW(Runtime::BuildForecast({}, matches, 0), std::invalid_argument);
    EXPECT_THROW(Runtime::BuildForecast(matches, {}, 0), std::invalid_argument);
}
