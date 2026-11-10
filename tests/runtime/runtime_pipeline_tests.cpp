// runtime_pipeline_tests.cpp
// Tests for the live pipeline stages: session clock, one-second aggregation, rolling window, and forecasting.

#include "core/inputconfig.hpp"
#include "runtime/forecast.hpp"
#include "runtime/liveinputwindow.hpp"
#include "runtime/marketclock.hpp"
#include "runtime/onesecondaggregator.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
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
            .record = Core::MLRecord{.mid_delta = midDelta/*, .spread_delta = 0.0f, .volume = 0.0f*/},
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

// ========== FORECAST ========== //

TEST(ForecastTests, AveragesCumulativePathsByWeight) {
    Runtime::HistoricalMatch up{.weight = 3.0};
    Runtime::HistoricalMatch down{.weight = 1.0};
    up.mid_deltas.fill(0.01);
    down.mid_deltas.fill(-0.01);

    const std::vector<Runtime::HistoricalMatch> matches{up, down};
    const Runtime::ForecastSummary summary = Runtime::BuildForecast(matches, 1234);

    EXPECT_EQ(summary.forecast.as_of_timestamp_ms, 1234);
    EXPECT_EQ(summary.forecast.historical_matches, 2u);
    EXPECT_NEAR(summary.effective_matches, 16.0 / 10.0, 1e-9);

    for (std::size_t i = 0; i < Runtime::PredictionEvaluationHorizons.size(); ++i) {
        const double horizon = static_cast<double>(Runtime::PredictionEvaluationHorizons[i]);

        // (3 × +0.01h + 1 × −0.01h) / 4 = +0.005h, while each path moves 0.01h in absolute terms.
        EXPECT_NEAR(summary.forecast.cumulative_changes[i], 0.005 * horizon, 1e-9);
        EXPECT_NEAR(summary.mean_absolute_moves[i], 0.01 * horizon, 1e-9);
    }
}

TEST(ForecastTests, CloserMatchesWeighMore) {
    EXPECT_GT(Runtime::MatchWeight(0.5f), Runtime::MatchWeight(1.0f));
    EXPECT_TRUE(std::isfinite(Runtime::MatchWeight(0.0f)));
}

TEST(ForecastTests, RejectsEmptyMatches) {
    EXPECT_THROW(Runtime::BuildForecast({}, 0), std::invalid_argument);
}
