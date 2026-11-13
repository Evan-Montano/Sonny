// runtime_pipeline_tests.cpp
// Tests for the live pipeline stages: session clock, one-second aggregation, rolling window, historical futures,
// forecasting, and the prediction experiment that ties them to the indexes.

#include "core/halfhourmultiindex.hpp"
#include "core/inputconfig.hpp"
#include "core/sessionmultiindex.hpp"
#include "core/transformer.hpp"
#include "runtime/forecast.hpp"
#include "runtime/historicalrecordstore.hpp"
#include "runtime/liveinputwindow.hpp"
#include "runtime/marketclock.hpp"
#include "runtime/onesecondaggregator.hpp"
#include "runtime/predictionexperiment.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
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

// ========== PREDICTION EXPERIMENT ========== //

/**
 * @brief Builds a small but complete storage layout (record files plus all 13 half-hour segments of
 * the chosen input windows) inside a temporary working directory, then drives the experiment with
 * snapshots cut from the historical days.
 */
class PredictionExperimentTests : public ::testing::Test {
protected:
    static constexpr std::size_t SEGMENT_COUNT = Core::SessionMultiIndex::SEGMENT_COUNT;
    static constexpr std::size_t SECONDS_PER_SEGMENT = Core::SessionMultiIndex::SECONDS_PER_SEGMENT;

    std::filesystem::path previousWorkingDirectory_;
    std::filesystem::path tempDirectory_;

    /**
     * @brief Trading days in the corpus, oldest first, with their records.
     */
    std::vector<std::pair<std::string, std::vector<Core::MLRecord>>> days_;

    void SetUp() override {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        tempDirectory_ = std::filesystem::temp_directory_path() / ("sonny_prediction_experiment_tests_" + std::to_string(timestamp));
        std::filesystem::create_directories(tempDirectory_ / "storage/records");

        previousWorkingDirectory_ = std::filesystem::current_path();
        std::filesystem::current_path(tempDirectory_);

        days_ = {{"2026-09-01", MakeDay(1)}, {"2026-09-02", MakeDay(2)}};

        for (const auto& [date, records] : days_) {
            std::ofstream file(std::filesystem::path("storage/records") / (date + ".rec"), std::ios::binary);
            file.write(
                reinterpret_cast<const char*>(records.data()),
                static_cast<std::streamsize>(records.size() * sizeof(Core::MLRecord))
            );
        }
    }

    void TearDown() override {
        std::filesystem::current_path(previousWorkingDirectory_);
        std::filesystem::remove_all(tempDirectory_);
    }

    /**
     * @brief A full session of deterministic signed half-cent moves, including flat seconds.
     */
    static std::vector<Core::MLRecord> MakeDay(std::uint32_t seed) {
        std::vector<Core::MLRecord> records;
        records.reserve(Runtime::SESSION_SECONDS);

        for (std::size_t i = 0; i < Runtime::SESSION_SECONDS; ++i) {
            seed = seed * 1103515245u + 12345u;
            const int direction = static_cast<int>((seed >> 16) % 3) - 1;
            const float size = 0.005f * static_cast<float>(1 + ((seed >> 8) % 40));

            records.push_back(Core::MLRecord{.mid_delta = static_cast<float>(direction) * size});
        }

        return records;
    }

    /**
     * @brief Mirrors Setup::ExecuteUpdateIndexes for one input window: every segment gets its own
     * normalization parameters and the windows that start inside it, newest day first.
     */
    void BuildIndexes(const std::size_t inputWindowSeconds) const {
        for (std::size_t segment = 0; segment < SEGMENT_COUNT; ++segment) {
            const std::size_t segmentOffset = segment * SECONDS_PER_SEGMENT;

            const auto segmentOf = [&](const std::vector<Core::MLRecord>& day) {
                const std::size_t end = std::min(segmentOffset + SECONDS_PER_SEGMENT + inputWindowSeconds - 1, day.size());
                return std::vector<Core::MLRecord>(
                    day.begin() + static_cast<std::ptrdiff_t>(segmentOffset),
                    day.begin() + static_cast<std::ptrdiff_t>(end)
                );
            };

            Core::NormalizationAccumulator accumulator{};
            for (const auto& [date, records] : days_) {
                for (Core::MLRecord record : segmentOf(records)) {
                    record.mid_delta = std::abs(record.mid_delta);
                    accumulator.AddRecord(record);
                }
            }

            Core::HalfHourMultiIndex index(segmentOffset, inputWindowSeconds);
            index.SetNormalizationParameters(accumulator.GetParameters());

            for (auto day = days_.rbegin(); day != days_.rend(); ++day) {
                std::vector<Core::MLRecord> records = segmentOf(day->second);
                index.AddVectorsToIndex(
                    std::span<Core::MLRecord>(records),
                    std::filesystem::path("storage/records") / (day->first + ".rec"),
                    segmentOffset
                );
            }
        }
    }

    /**
     * @brief A live snapshot whose newest records are the historical seconds ending at historicalLastSecond,
     * stamped as if its newest record were liveLastSecond seconds after Thursday's open.
     */
    static Runtime::LiveInputWindow::Snapshot MakeSnapshot(
        const std::vector<Core::MLRecord>& day,
        const std::size_t historicalLastSecond,
        const std::size_t liveLastSecond
    ) {
        const std::size_t first = historicalLastSecond + 1 - Core::MAX_INPUT_WINDOW_SECONDS;

        return Runtime::LiveInputWindow::Snapshot{
            .records = std::vector<Core::MLRecord>(
                day.begin() + static_cast<std::ptrdiff_t>(first),
                day.begin() + static_cast<std::ptrdiff_t>(historicalLastSecond + 1)
            ),
            .last_timestamp_ms = THURSDAY_OPEN_MS + static_cast<std::int64_t>(liveLastSecond) * 1000
        };
    }

    static bool HasEveryForecast(const Runtime::PredictionExperiment::ForecastRow& row) {
        return std::all_of(row.begin(), row.end(), [](const auto& forecast) { return forecast.has_value(); });
    }

    static bool HasNoForecast(const Runtime::PredictionExperiment::ForecastRow& row) {
        return std::none_of(row.begin(), row.end(), [](const auto& forecast) { return forecast.has_value(); });
    }
};

TEST_F(PredictionExperimentTests, SingleWindowModeForecastsOnlyTheSelectedWindow) {
    BuildIndexes(10);
    BuildIndexes(15);

    Runtime::PredictionExperiment experiment(Core::SelectInputWindows(false, 15));
    EXPECT_EQ(experiment.InputWindows(), std::vector<std::size_t>{15});

    const auto rows = experiment.RunSnapshot(MakeSnapshot(days_[0].second, 5000, 5000));

    // One consolidated row for the window, not one per half-hour segment.
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_TRUE(HasEveryForecast(rows[0]));

    // The cap is applied to the pooled matches: each forecast uses exactly its match count.
    for (std::size_t i = 0; i < Runtime::PREDICTION_MATCH_COUNTS.size(); ++i) {
        EXPECT_EQ(rows[0][i]->historical_matches, Runtime::PREDICTION_MATCH_COUNTS[i]);
    }
}

TEST_F(PredictionExperimentTests, AllWindowsModeForecastsEachConfiguredWindowIndependently) {
    // Only two of the configured windows have indexes.
    BuildIndexes(10);
    BuildIndexes(15);

    const Runtime::LiveInputWindow::Snapshot snapshot = MakeSnapshot(days_[0].second, 5000, 5000);

    Runtime::PredictionExperiment allWindows(Core::SelectInputWindows(true, 60));
    const auto rows = allWindows.RunSnapshot(snapshot);

    ASSERT_EQ(rows.size(), Core::INPUT_WINDOW_CONFIGURATIONS.size());
    ASSERT_EQ(allWindows.InputWindows()[0], 10u);
    ASSERT_EQ(allWindows.InputWindows()[1], 15u);

    // Windows without indexes are unavailable; they do not stop or alter the others.
    EXPECT_TRUE(HasEveryForecast(rows[0]));
    EXPECT_TRUE(HasEveryForecast(rows[1]));
    for (std::size_t window = 2; window < rows.size(); ++window) {
        EXPECT_TRUE(HasNoForecast(rows[window])) << allWindows.InputWindows()[window] << "s";
    }

    // Each window's forecast is exactly what it produces when run alone.
    for (const std::size_t slot : {std::size_t{0}, std::size_t{1}}) {
        Runtime::PredictionExperiment alone({allWindows.InputWindows()[slot]});
        const auto aloneRows = alone.RunSnapshot(snapshot);

        ASSERT_EQ(aloneRows.size(), 1u);
        for (std::size_t i = 0; i < Runtime::PREDICTION_MATCH_COUNTS.size(); ++i) {
            ASSERT_TRUE(aloneRows[0][i].has_value());
            EXPECT_EQ(aloneRows[0][i]->cumulative_changes, rows[slot][i]->cumulative_changes);
        }
    }

    // The two windows are different searches, not copies of one result.
    EXPECT_NE(rows[0].back()->cumulative_changes, rows[1].back()->cumulative_changes);
}

TEST_F(PredictionExperimentTests, MatchesComeFromAnySegmentWhateverTheTimeOfDay) {
    BuildIndexes(10);

    // The live window repeats a historical window from the 14:30-15:00 segment (segment 10),
    // but arrives a few minutes after the open (segment 0).
    const std::vector<Core::MLRecord>& day = days_[1].second;
    const std::size_t historicalLastSecond = 10 * SECONDS_PER_SEGMENT + 700;

    Runtime::PredictionExperiment experiment({10});
    const auto rows = experiment.RunSnapshot(MakeSnapshot(day, historicalLastSecond, 300));

    ASSERT_EQ(rows.size(), 1u);
    ASSERT_TRUE(HasEveryForecast(rows[0]));

    // An exact magnitude match outweighs every other match by many orders of magnitude, so the
    // forecast's size is the size of what followed that window. It is only found if segment 10 was searched.
    std::size_t comparedHorizons = 0;

    for (std::size_t i = 0; i < Runtime::PredictionEvaluationHorizons.size(); ++i) {
        double cumulative = 0.0;
        for (std::size_t second = 1; second <= Runtime::PredictionEvaluationHorizons[i]; ++second) {
            cumulative += day[historicalLastSecond + second].mid_delta;
        }

        // A split direction vote zeroes the forecast; its size cannot be read back then.
        const double forecast = rows[0].back()->cumulative_changes[i];
        if (forecast != 0.0) {
            EXPECT_NEAR(std::abs(forecast), std::abs(cumulative), 1e-6) << Runtime::PredictionEvaluationHorizons[i] << "s";
            ++comparedHorizons;
        }
    }

    EXPECT_GT(comparedHorizons, 0u);
}

TEST_F(PredictionExperimentTests, MissingSegmentsDoNotStopTheForecast) {
    BuildIndexes(10);

    // Segment 3 loses a file, and segment 8 loses everything.
    std::filesystem::remove("storage/indexes/10s/d/1100-1130.index");
    for (const char* type : {"m", "d"}) {
        std::filesystem::remove(std::format("storage/indexes/10s/{}/1330-1400.index", type));
        std::filesystem::remove(std::format("storage/metadata/10s/{}/1330-1400.meta", type));
    }

    Runtime::PredictionExperiment experiment({10});
    const auto rows = experiment.RunSnapshot(MakeSnapshot(days_[0].second, 5000, 5000));

    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(HasEveryForecast(rows[0]));

    // The missing segments were skipped, not recreated empty.
    EXPECT_FALSE(std::filesystem::exists("storage/indexes/10s/d/1100-1130.index"));
    EXPECT_FALSE(std::filesystem::exists("storage/indexes/10s/m/1330-1400.index"));
}

TEST_F(PredictionExperimentTests, WindowWithoutAnyIndexIsUnavailable) {
    Runtime::PredictionExperiment experiment({10});

    const Runtime::LiveInputWindow::Snapshot snapshot = MakeSnapshot(days_[0].second, 5000, 5000);
    const auto rows = experiment.RunSnapshot(snapshot);

    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(HasNoForecast(rows[0]));
    EXPECT_FALSE(std::filesystem::exists("storage/indexes"));

    // Later cycles stay unavailable without failing.
    EXPECT_TRUE(HasNoForecast(experiment.RunSnapshot(snapshot)[0]));
}

TEST_F(PredictionExperimentTests, SkipsSnapshotsOutsideTheSession) {
    BuildIndexes(10);

    Runtime::PredictionExperiment experiment({10});

    // Pre-market: no historical session to compare against.
    Runtime::LiveInputWindow::Snapshot preMarket = MakeSnapshot(days_[0].second, 5000, 5000);
    preMarket.last_timestamp_ms = THURSDAY_OPEN_MS - 1000;
    EXPECT_TRUE(experiment.RunSnapshot(preMarket).empty());

    // A window that still reaches back before the open has no row filled in.
    const auto earlyRows = experiment.RunSnapshot(MakeSnapshot(days_[0].second, 5000, 5));
    ASSERT_EQ(earlyRows.size(), 1u);
    EXPECT_TRUE(HasNoForecast(earlyRows[0]));
}

TEST_F(PredictionExperimentTests, RejectsAnUnconfiguredInputWindow) {
    EXPECT_THROW(Runtime::PredictionExperiment({25}), std::invalid_argument);
}
