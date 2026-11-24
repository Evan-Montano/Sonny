// runtime_pipeline_tests.cpp
// Tests for the live pipeline stages: session clock, one-second aggregation, rolling window, historical futures,
// forecasting, and the prediction experiment that ties them to the indexes.

#include "common/datetime.hpp"
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
        quote.Symbol = "SPY";
        quote.TimestampMs = timestampMs;
        quote.Bids.push_back({.Price = bid, .Size = 100});
        quote.Asks.push_back({.Price = ask, .Size = 100});
        return quote;
    }

    /**
     * @brief Collects everything an aggregator emits.
     */
    struct AggregatorHarness {
        // MEMBERS

        std::vector<Runtime::LiveRecord> Records;
        int Resets = 0;

        Runtime::OneSecondAggregator Aggregator{
            [this](const Runtime::LiveRecord& record) { Records.push_back(record); },
            [this] { ++Resets; }
        };

        // METHODS

        // Sends one quote at the start of each listed second (relative to the open) with the given midpoint.
        void Send(const std::int64_t second, const double mid, const std::int64_t offsetMs = 0) {
            Aggregator.AddQuote(MakeQuote(THURSDAY_OPEN_MS + second * 1000 + offsetMs, mid - 0.01, mid + 0.01));
        }
    };

    Runtime::LiveRecord MakeLiveRecord(const std::int64_t second, const float midDelta = 0.0f) {
        return Runtime::LiveRecord{
            .Record = Core::MLRecord{.MidDelta = midDelta},
            .TimestampMs = second * 1000,
            .Observed = true
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

TEST(MarketClockTests, SignedMillisecondsKeepTheSessionBoundaryBeforeTheEpoch) {
    const Common::DateTime open(1969, Common::DEC, 29, 9, 30, 0, Common::TimeZone::NewYork);
    const auto timestamp = open.GetTimestampMilliseconds();
    EXPECT_FALSE(Runtime::GetSecondsSinceMarketOpen(timestamp - 1).has_value());
    EXPECT_EQ(Runtime::GetSecondsSinceMarketOpen(timestamp), 0u);
    EXPECT_EQ(Runtime::GetSecondsSinceMarketOpen(timestamp + 999), 0u);
    EXPECT_EQ(Runtime::GetSecondsSinceMarketOpen(timestamp + 1000), 1u);
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

    ASSERT_EQ(harness.Records.size(), 1u);
    EXPECT_NEAR(harness.Records[0].Record.MidDelta, 0.10, 1e-4);
    EXPECT_EQ(harness.Records[0].TimestampMs / 1000, THURSDAY_OPEN_MS / 1000 + 1);
    EXPECT_TRUE(harness.Records[0].Observed);
}

TEST(OneSecondAggregatorTests, QuietSecondsCarryThePriceForwardWithZeroDeltas) {
    AggregatorHarness harness;

    harness.Send(0, 100.00);
    harness.Send(1, 100.10);
    harness.Send(4, 100.40);          // Seconds 2 and 3 are quiet.
    harness.Send(5, 100.40);          // Completes second 4.

    ASSERT_EQ(harness.Records.size(), 4u);

    double cumulative = 0.0;
    for (std::size_t i = 0; i < harness.Records.size(); ++i) {
        EXPECT_EQ(harness.Records[i].TimestampMs / 1000, THURSDAY_OPEN_MS / 1000 + 1 + static_cast<std::int64_t>(i));
        cumulative += harness.Records[i].Record.MidDelta;
    }

    EXPECT_FALSE(harness.Records[1].Observed);
    EXPECT_FALSE(harness.Records[2].Observed);
    EXPECT_FLOAT_EQ(harness.Records[1].Record.MidDelta, 0.0f);
    EXPECT_FLOAT_EQ(harness.Records[2].Record.MidDelta, 0.0f);

    // The full move is attributed to second 4, so the cumulative path matches the real price.
    EXPECT_NEAR(harness.Records[3].Record.MidDelta, 0.30, 1e-4);
    EXPECT_NEAR(cumulative, 0.40, 1e-4);
    EXPECT_EQ(harness.Resets, 0);
}

TEST(OneSecondAggregatorTests, LongInterruptionsResetInsteadOfFillingTheGap) {
    AggregatorHarness harness;

    harness.Send(0, 100.00);
    harness.Send(1, 100.10);
    harness.Send(2, 100.20);
    const std::size_t recordsBeforeGap = harness.Records.size();

    harness.Send(2 + Runtime::OneSecondAggregator::MAX_CARRIED_FORWARD_SECONDS + 2, 101.00);

    EXPECT_EQ(harness.Resets, 1);
    EXPECT_EQ(harness.Records.size(), recordsBeforeGap);
}

TEST(OneSecondAggregatorTests, OneSidedBookResetsContinuity) {
    AggregatorHarness harness;

    harness.Send(0, 100.00);
    harness.Send(1, 100.10);

    Client::WebullQuote oneSided = MakeQuote(THURSDAY_OPEN_MS + 2000, 100.0, 100.2);
    oneSided.Asks.clear();
    harness.Aggregator.AddQuote(oneSided);

    harness.Send(3, 100.30);          // Completes the one-sided second 2.
    EXPECT_EQ(harness.Resets, 1);

    // Second 3 becomes the new baseline, so the next record is second 4's move from it.
    harness.Send(4, 100.35);
    harness.Send(5, 100.35);
    ASSERT_FALSE(harness.Records.empty());
    EXPECT_NEAR(harness.Records.back().Record.MidDelta, 0.05, 1e-4);
}

TEST(OneSecondAggregatorTests, IgnoresQuotesFromAnEarlierSecond) {
    AggregatorHarness harness;

    harness.Send(0, 100.00);
    harness.Send(1, 100.10);
    harness.Send(0, 99.00, 500);      // Late, already finalized.
    harness.Send(2, 100.10);

    ASSERT_EQ(harness.Records.size(), 1u);
    EXPECT_NEAR(harness.Records[0].Record.MidDelta, 0.10, 1e-4);
}

// ========== LIVE INPUT WINDOW ========== //

TEST(LiveInputWindowTests, SnapshotsOnlyFullUnchangedWindowsOnce) {
    Runtime::LiveInputWindow window;

    for (std::int64_t second = 0; second < static_cast<std::int64_t>(Core::LIVE_INPUT_WINDOW_SECONDS) - 1; ++second) {
        window.Push(MakeLiveRecord(second));
    }
    EXPECT_FALSE(window.TakeSnapshotIfUpdated().has_value());

    window.Push(MakeLiveRecord(static_cast<std::int64_t>(Core::LIVE_INPUT_WINDOW_SECONDS) - 1, 0.5f));

    const std::optional<Runtime::LiveInputWindow::Snapshot> snapshot = window.TakeSnapshotIfUpdated();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->Records.size(), Core::LIVE_INPUT_WINDOW_SECONDS);
    EXPECT_FLOAT_EQ(snapshot->Records.back().MidDelta, 0.5f);
    EXPECT_EQ(snapshot->LastTimestampMs, (static_cast<std::int64_t>(Core::LIVE_INPUT_WINDOW_SECONDS) - 1) * 1000);

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
    // MEMBERS

    std::filesystem::path PreviousWorkingDirectory;
    std::filesystem::path TempDirectory;

    // METHODS

    void SetUp() override {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        TempDirectory = std::filesystem::temp_directory_path() / ("sonny_record_store_tests_" + std::to_string(timestamp));
        std::filesystem::create_directories(TempDirectory / "storage/records");

        PreviousWorkingDirectory = std::filesystem::current_path();
        std::filesystem::current_path(TempDirectory);
    }

    void TearDown() override {
        std::filesystem::current_path(PreviousWorkingDirectory);
        std::filesystem::remove_all(TempDirectory);
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
    EXPECT_FLOAT_EQ((*day)[1].MidDelta, 0.01f);
    EXPECT_FLOAT_EQ((*day)[2].MidDelta, -0.02f);

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
    Runtime::HistoricalMatch up{.Weight = 3.0};
    Runtime::HistoricalMatch down{.Weight = 1.0};
    up.MidDeltas.fill(0.01);
    down.MidDeltas.fill(-0.01);

    // Direction comes only from the direction matches, which all move up.
    Runtime::HistoricalMatch directionUp{.Weight = 1.0};
    directionUp.MidDeltas.fill(0.02);

    const std::vector<Runtime::HistoricalMatch> magnitudeMatches{up, down};
    const std::vector<Runtime::HistoricalMatch> directionMatches{directionUp, directionUp};
    const Runtime::ForecastSummary summary = Runtime::BuildForecast(magnitudeMatches, directionMatches, 1234);

    EXPECT_EQ(summary.Forecast.AsOfTimestampMs, 1234);
    EXPECT_EQ(summary.Forecast.HistoricalMatches, 2u);
    EXPECT_EQ(summary.MagnitudeMatches, 2u);
    EXPECT_EQ(summary.DirectionMatches, 2u);
    EXPECT_NEAR(summary.MagnitudeEffectiveMatches, 16.0 / 10.0, 1e-9);
    EXPECT_NEAR(summary.DirectionEffectiveMatches, 2.0, 1e-9);

    for (std::size_t i = 0; i < Runtime::PredictionEvaluationHorizons.size(); ++i) {
        const double horizon = static_cast<double>(Runtime::PredictionEvaluationHorizons[i]);

        // Both magnitude paths move 0.01h in absolute terms, so the expected magnitude is 0.01h,
        // and the direction matches unanimously vote up.
        EXPECT_NEAR(summary.ExpectedMagnitudes[i], 0.01 * horizon, 1e-9);
        EXPECT_NEAR(summary.DirectionAgreement[i], 1.0, 1e-9);
        EXPECT_NEAR(summary.Forecast.CumulativeChanges[i], 0.01 * horizon, 1e-9);
    }
}

TEST(ForecastTests, DirectionFollowsTheWeightedVote) {
    Runtime::HistoricalMatch magnitude{.Weight = 1.0};
    magnitude.MidDeltas.fill(0.01);

    Runtime::HistoricalMatch down{.Weight = 3.0};
    Runtime::HistoricalMatch up{.Weight = 1.0};
    down.MidDeltas.fill(-0.01);
    up.MidDeltas.fill(0.05);

    const std::vector<Runtime::HistoricalMatch> magnitudeMatches{magnitude};
    const std::vector<Runtime::HistoricalMatch> directionMatches{down, up};
    const Runtime::ForecastSummary summary = Runtime::BuildForecast(magnitudeMatches, directionMatches, 0);

    for (std::size_t i = 0; i < Runtime::PredictionEvaluationHorizons.size(); ++i) {
        const double horizon = static_cast<double>(Runtime::PredictionEvaluationHorizons[i]);

        // Votes are signs, not sizes: (3 × −1 + 1 × +1) / 4 = −0.5, despite "up" moving further.
        EXPECT_NEAR(summary.DirectionAgreement[i], -0.5, 1e-9);
        EXPECT_NEAR(summary.Forecast.CumulativeChanges[i], -0.01 * horizon, 1e-9);
    }
}

TEST(ForecastTests, SplitDirectionVoteForecastsNoChange) {
    Runtime::HistoricalMatch magnitude{.Weight = 1.0};
    magnitude.MidDeltas.fill(0.01);

    Runtime::HistoricalMatch up{.Weight = 1.0};
    Runtime::HistoricalMatch down{.Weight = 1.0};
    up.MidDeltas.fill(0.01);
    down.MidDeltas.fill(-0.01);

    const std::vector<Runtime::HistoricalMatch> magnitudeMatches{magnitude};
    const std::vector<Runtime::HistoricalMatch> directionMatches{up, down};
    const Runtime::ForecastSummary summary = Runtime::BuildForecast(magnitudeMatches, directionMatches, 0);

    for (std::size_t i = 0; i < Runtime::PredictionEvaluationHorizons.size(); ++i) {
        EXPECT_DOUBLE_EQ(summary.DirectionAgreement[i], 0.0);
        EXPECT_DOUBLE_EQ(summary.Forecast.CumulativeChanges[i], 0.0);
        EXPECT_GT(summary.ExpectedMagnitudes[i], 0.0);
    }
}

TEST(ForecastTests, FlatDirectionMatchesDoNotVote) {
    Runtime::HistoricalMatch magnitude{.Weight = 1.0};
    magnitude.MidDeltas.fill(0.01);

    // Cumulative moves below FLAT_MOVE_EPSILON count as flat for every horizon.
    Runtime::HistoricalMatch flat{.Weight = 10.0};
    flat.MidDeltas.fill(Runtime::FLAT_MOVE_EPSILON / 100.0);
    Runtime::HistoricalMatch up{.Weight = 1.0};
    up.MidDeltas.fill(0.01);

    const std::vector<Runtime::HistoricalMatch> magnitudeMatches{magnitude};
    const std::vector<Runtime::HistoricalMatch> directionMatches{flat, up};
    const Runtime::ForecastSummary summary = Runtime::BuildForecast(magnitudeMatches, directionMatches, 0);

    for (std::size_t i = 0; i < Runtime::PredictionEvaluationHorizons.size(); ++i) {
        EXPECT_NEAR(summary.DirectionAgreement[i], 1.0 / 11.0, 1e-9);
        EXPECT_GT(summary.Forecast.CumulativeChanges[i], 0.0);
    }
}

TEST(ForecastTests, CloserMatchesWeighMore) {
    EXPECT_GT(Runtime::MatchWeight(0.5f), Runtime::MatchWeight(1.0f));
    EXPECT_TRUE(std::isfinite(Runtime::MatchWeight(0.0f)));

    EXPECT_GT(Runtime::DirectionMatchWeight(0.0f), Runtime::DirectionMatchWeight(1.0f));
    EXPECT_DOUBLE_EQ(Runtime::DirectionMatchWeight(0.0f), 1.0);
}

TEST(ForecastTests, RejectsEmptyMatches) {
    Runtime::HistoricalMatch match{.Weight = 1.0};
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
    // MEMBERS

    static constexpr std::size_t WINDOW = Core::INPUT_WINDOW_CONFIGURATIONS.front();
    static constexpr std::size_t OTHER_WINDOW = Core::INPUT_WINDOW_CONFIGURATIONS[1];
    static constexpr std::size_t SEGMENT_COUNT = Core::SessionMultiIndex::SEGMENT_COUNT;
    static constexpr std::size_t SECONDS_PER_SEGMENT = Core::SessionMultiIndex::SECONDS_PER_SEGMENT;

    std::filesystem::path PreviousWorkingDirectory;
    std::filesystem::path TempDirectory;

    /**
     * @brief Trading days in the corpus, oldest first, with their records.
     */
    std::vector<std::pair<std::string, std::vector<Core::MLRecord>>> Days;

    // METHODS

    void SetUp() override {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        TempDirectory = std::filesystem::temp_directory_path() / ("sonny_prediction_experiment_tests_" + std::to_string(timestamp));
        std::filesystem::create_directories(TempDirectory / "storage/records");

        PreviousWorkingDirectory = std::filesystem::current_path();
        std::filesystem::current_path(TempDirectory);

        Days = {{"2026-09-01", MakeDay(1)}, {"2026-09-02", MakeDay(2)}};

        for (const auto& [date, records] : Days) {
            std::ofstream file(std::filesystem::path("storage/records") / (date + ".rec"), std::ios::binary);
            file.write(
                reinterpret_cast<const char*>(records.data()),
                static_cast<std::streamsize>(records.size() * sizeof(Core::MLRecord))
            );
        }
    }

    void TearDown() override {
        std::filesystem::current_path(PreviousWorkingDirectory);
        std::filesystem::remove_all(TempDirectory);
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

            records.push_back(Core::MLRecord{.MidDelta = static_cast<float>(direction) * size});
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

            Core::HalfHourMultiIndex index(segmentOffset, inputWindowSeconds);

            Core::NormalizationAccumulator accumulator{};
            for (const auto& [date, records] : Days) {
                for (Core::MLRecord record : segmentOf(records)) {
                    record.MidDelta = std::abs(record.MidDelta);
                    accumulator.AddRecord(record);
                }
            }

            index.SetNormalizationParameters(accumulator.GetParameters());

            for (auto day = Days.rbegin(); day != Days.rend(); ++day) {
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
            .Records = std::vector<Core::MLRecord>(
                day.begin() + static_cast<std::ptrdiff_t>(first),
                day.begin() + static_cast<std::ptrdiff_t>(historicalLastSecond + 1)
            ),
            .LastTimestampMs = THURSDAY_OPEN_MS + static_cast<std::int64_t>(liveLastSecond) * 1000
        };
    }

    static bool HasEveryForecast(const Runtime::PredictionExperiment::ForecastRow& row) {
        return std::all_of(row.begin(), row.end(), [](const auto& forecast) { return forecast.has_value(); });
    }

    static bool HasNoForecast(const Runtime::PredictionExperiment::ForecastRow& row) {
        return std::none_of(row.begin(), row.end(), [](const auto& forecast) { return forecast.has_value(); });
    }

    /**
     * @brief True when the forecast's sizes are those of what followed the given historical window.
     * An exact magnitude match outweighs every other match by many orders of magnitude, so this
     * holds only if the segment containing that window was searched.
     */
    static bool FoundExactMatch(
        const Runtime::PredictionExperiment::ForecastRow& row,
        const std::vector<Core::MLRecord>& day,
        const std::size_t historicalLastSecond
    ) {
        std::size_t comparedHorizons = 0;

        for (std::size_t i = 0; i < Runtime::PredictionEvaluationHorizons.size(); ++i) {
            double cumulative = 0.0;
            for (std::size_t second = 1; second <= Runtime::PredictionEvaluationHorizons[i]; ++second) {
                cumulative += day[historicalLastSecond + second].MidDelta;
            }

            // A split direction vote zeroes the forecast; its size cannot be read back then.
            const double forecast = row.back()->CumulativeChanges[i];
            if (forecast != 0.0) {
                if (std::abs(std::abs(forecast) - std::abs(cumulative)) > 1e-6) return false;
                ++comparedHorizons;
            }
        }

        return comparedHorizons > 0;
    }
};

TEST_F(PredictionExperimentTests, SingleWindowModeForecastsOnlyTheSelectedWindow) {
    BuildIndexes(WINDOW);
    BuildIndexes(OTHER_WINDOW);

    Runtime::PredictionExperiment experiment(Core::SelectInputWindows(false, OTHER_WINDOW), SEGMENT_COUNT);
    EXPECT_EQ(experiment.InputWindows(), std::vector<std::size_t>{OTHER_WINDOW});

    const auto rows = experiment.RunSnapshot(MakeSnapshot(Days[0].second, 5000, 5000));

    // One consolidated row for the window, not one per half-hour segment.
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_TRUE(HasEveryForecast(rows[0]));

    // The cap is applied to the pooled matches: each forecast uses exactly its match count.
    for (std::size_t i = 0; i < Runtime::PREDICTION_MATCH_COUNTS.size(); ++i) {
        EXPECT_EQ(rows[0][i]->HistoricalMatches, Runtime::PREDICTION_MATCH_COUNTS[i]);
    }
}

TEST_F(PredictionExperimentTests, AllWindowsModeForecastsEachConfiguredWindowIndependently) {
    // Only two of the configured windows have indexes.
    BuildIndexes(WINDOW);
    BuildIndexes(OTHER_WINDOW);

    const Runtime::LiveInputWindow::Snapshot snapshot = MakeSnapshot(Days[0].second, 5000, 5000);

    Runtime::PredictionExperiment allWindows(Core::SelectInputWindows(true, Core::MAX_INPUT_WINDOW_SECONDS), SEGMENT_COUNT);
    const auto rows = allWindows.RunSnapshot(snapshot);

    ASSERT_EQ(rows.size(), Core::INPUT_WINDOW_CONFIGURATIONS.size());
    ASSERT_EQ(allWindows.InputWindows()[0], WINDOW);
    ASSERT_EQ(allWindows.InputWindows()[1], OTHER_WINDOW);

    // Windows without indexes are unavailable; they do not stop or alter the others.
    EXPECT_TRUE(HasEveryForecast(rows[0]));
    EXPECT_TRUE(HasEveryForecast(rows[1]));
    for (std::size_t window = 2; window < rows.size(); ++window) {
        EXPECT_TRUE(HasNoForecast(rows[window])) << allWindows.InputWindows()[window] << "s";
    }

    // Each window's forecast is exactly what it produces when run alone.
    for (const std::size_t slot : {std::size_t{0}, std::size_t{1}}) {
        Runtime::PredictionExperiment alone({allWindows.InputWindows()[slot]}, SEGMENT_COUNT);
        const auto aloneRows = alone.RunSnapshot(snapshot);

        ASSERT_EQ(aloneRows.size(), 1u);
        for (std::size_t i = 0; i < Runtime::PREDICTION_MATCH_COUNTS.size(); ++i) {
            ASSERT_TRUE(aloneRows[0][i].has_value());
            EXPECT_EQ(aloneRows[0][i]->CumulativeChanges, rows[slot][i]->CumulativeChanges);
        }
    }

    // The two windows are different searches, not copies of one result.
    EXPECT_NE(rows[0].back()->CumulativeChanges, rows[1].back()->CumulativeChanges);
}

TEST_F(PredictionExperimentTests, MatchesComeFromAnySegmentWhateverTheTimeOfDay) {
    BuildIndexes(WINDOW);

    // The live window repeats a historical window from the 14:30-15:00 segment (segment 10),
    // but arrives a few minutes after the open (segment 0).
    const std::vector<Core::MLRecord>& day = Days[1].second;
    const std::size_t historicalLastSecond = 10 * SECONDS_PER_SEGMENT + 700;

    Runtime::PredictionExperiment experiment({WINDOW}, SEGMENT_COUNT);
    const auto rows = experiment.RunSnapshot(MakeSnapshot(day, historicalLastSecond, 300));

    ASSERT_EQ(rows.size(), 1u);
    ASSERT_TRUE(HasEveryForecast(rows[0]));

    // An exact magnitude match outweighs every other match by many orders of magnitude, so the
    // forecast's size is the size of what followed that window. It is only found if segment 10 was searched.
    std::size_t comparedHorizons = 0;

    for (std::size_t i = 0; i < Runtime::PredictionEvaluationHorizons.size(); ++i) {
        double cumulative = 0.0;
        for (std::size_t second = 1; second <= Runtime::PredictionEvaluationHorizons[i]; ++second) {
            cumulative += day[historicalLastSecond + second].MidDelta;
        }

        // A split direction vote zeroes the forecast; its size cannot be read back then.
        const double forecast = rows[0].back()->CumulativeChanges[i];
        if (forecast != 0.0) {
            EXPECT_NEAR(std::abs(forecast), std::abs(cumulative), 1e-6) << Runtime::PredictionEvaluationHorizons[i] << "s";
            ++comparedHorizons;
        }
    }

    EXPECT_GT(comparedHorizons, 0u);
}

TEST_F(PredictionExperimentTests, MissingSegmentsDoNotStopTheForecast) {
    BuildIndexes(WINDOW);

    // Segment 3 loses a file, and segment 8 loses everything.
    std::filesystem::remove(std::format("storage/indexes/{}s/d/1100-1130.index", WINDOW));
    for (const char* type : {"m", "d"}) {
        std::filesystem::remove(std::format("storage/indexes/{}s/{}/1330-1400.index", WINDOW, type));
        std::filesystem::remove(std::format("storage/metadata/{}s/{}/1330-1400.meta", WINDOW, type));
    }

    Runtime::PredictionExperiment experiment({WINDOW}, SEGMENT_COUNT);
    const auto rows = experiment.RunSnapshot(MakeSnapshot(Days[0].second, 5000, 5000));

    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(HasEveryForecast(rows[0]));

    // The missing segments were skipped, not recreated empty.
    EXPECT_FALSE(std::filesystem::exists(std::format("storage/indexes/{}s/d/1100-1130.index", WINDOW)));
    EXPECT_FALSE(std::filesystem::exists(std::format("storage/indexes/{}s/m/1330-1400.index", WINDOW)));
}

TEST_F(PredictionExperimentTests, WindowWithoutAnyIndexIsUnavailable) {
    Runtime::PredictionExperiment experiment({WINDOW}, SEGMENT_COUNT);

    const Runtime::LiveInputWindow::Snapshot snapshot = MakeSnapshot(Days[0].second, 5000, 5000);
    const auto rows = experiment.RunSnapshot(snapshot);

    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(HasNoForecast(rows[0]));
    EXPECT_FALSE(std::filesystem::exists("storage/indexes"));

    // Later cycles stay unavailable without failing.
    EXPECT_TRUE(HasNoForecast(experiment.RunSnapshot(snapshot)[0]));
}

TEST_F(PredictionExperimentTests, SkipsSnapshotsOutsideTheSession) {
    BuildIndexes(WINDOW);

    Runtime::PredictionExperiment experiment({WINDOW}, SEGMENT_COUNT);

    // Pre-market: no historical session to compare against.
    Runtime::LiveInputWindow::Snapshot preMarket = MakeSnapshot(Days[0].second, 5000, 5000);
    preMarket.LastTimestampMs = THURSDAY_OPEN_MS - 1000;
    EXPECT_TRUE(experiment.RunSnapshot(preMarket).empty());

    // A window that still reaches back before the open has no row filled in.
    const auto earlyRows = experiment.RunSnapshot(MakeSnapshot(Days[0].second, 5000, 5));
    ASSERT_EQ(earlyRows.size(), 1u);
    EXPECT_TRUE(HasNoForecast(earlyRows[0]));
}

TEST_F(PredictionExperimentTests, RejectsAnUnconfiguredInputWindow) {
    EXPECT_THROW(Runtime::PredictionExperiment({25}), std::invalid_argument);
}

TEST_F(PredictionExperimentTests, RejectsALimitOfNoSegments) {
    EXPECT_THROW(Runtime::PredictionExperiment({WINDOW}, 0), std::invalid_argument);
}

// === SEGMENT LIMIT AND PRELOAD === //

TEST_F(PredictionExperimentTests, LimitedSegmentsAreTheOnesNearestTheTimeOfDay) {
    BuildIndexes(WINDOW);

    // The live window repeats a historical window from the 14:30-15:00 segment (segment 10).
    const std::vector<Core::MLRecord>& day = Days[1].second;
    const std::size_t historicalLastSecond = 10 * SECONDS_PER_SEGMENT + 700;

    const std::size_t afterTheOpen = 300;
    const std::size_t lastHalfHour = 12 * SECONDS_PER_SEGMENT + 100;

    // Eight segments in the last half hour are 12:00 to the close, which includes segment 10.
    {
        Runtime::PredictionExperiment experiment({WINDOW}, 8);
        const auto rows = experiment.RunSnapshot(MakeSnapshot(day, historicalLastSecond, lastHalfHour));
        ASSERT_TRUE(HasEveryForecast(rows[0]));
        EXPECT_TRUE(FoundExactMatch(rows[0], day, historicalLastSecond));
    }

    // Eight segments after the open are 09:30 to 13:30, which does not.
    {
        Runtime::PredictionExperiment experiment({WINDOW}, 8);
        const auto rows = experiment.RunSnapshot(MakeSnapshot(day, historicalLastSecond, afterTheOpen));
        ASSERT_TRUE(HasEveryForecast(rows[0]));
        EXPECT_FALSE(FoundExactMatch(rows[0], day, historicalLastSecond));
    }

    // A tighter limit late in the day still reaches one segment back.
    {
        Runtime::PredictionExperiment experiment({WINDOW}, 4);
        const auto rows = experiment.RunSnapshot(MakeSnapshot(day, historicalLastSecond, 11 * SECONDS_PER_SEGMENT + 100));
        ASSERT_TRUE(HasEveryForecast(rows[0]));
        EXPECT_TRUE(FoundExactMatch(rows[0], day, historicalLastSecond));
    }
}

TEST_F(PredictionExperimentTests, LoadedSegmentsMoveWithTheSession) {
    BuildIndexes(WINDOW);

    const std::vector<Core::MLRecord>& day = Days[1].second;
    const std::size_t historicalLastSecond = 10 * SECONDS_PER_SEGMENT + 700;

    Runtime::PredictionExperiment experiment({WINDOW}, 2);

    // Early on, segment 10 is far outside the two loaded segments.
    auto rows = experiment.RunSnapshot(MakeSnapshot(day, historicalLastSecond, 300));
    ASSERT_TRUE(HasEveryForecast(rows[0]));
    EXPECT_FALSE(FoundExactMatch(rows[0], day, historicalLastSecond));

    // Once the session reaches segment 10, the loaded pair has moved there.
    rows = experiment.RunSnapshot(MakeSnapshot(day, historicalLastSecond, 10 * SECONDS_PER_SEGMENT + 900));
    ASSERT_TRUE(HasEveryForecast(rows[0]));
    EXPECT_TRUE(FoundExactMatch(rows[0], day, historicalLastSecond));
}

TEST_F(PredictionExperimentTests, PreloadReadsTheIndexesBeforeTheFirstSnapshot) {
    BuildIndexes(WINDOW);

    const Runtime::LiveInputWindow::Snapshot snapshot = MakeSnapshot(Days[0].second, 5000, 5000);

    Runtime::PredictionExperiment preloaded({WINDOW}, 3);
    preloaded.Preload(snapshot.LastTimestampMs);

    // With the index files gone, only an experiment that already holds them can forecast.
    std::filesystem::remove_all("storage/indexes");
    std::filesystem::remove_all("storage/metadata");

    const auto rows = preloaded.RunSnapshot(snapshot);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(HasEveryForecast(rows[0]));

    Runtime::PredictionExperiment notPreloaded({WINDOW}, 3);
    EXPECT_TRUE(HasNoForecast(notPreloaded.RunSnapshot(snapshot)[0]));
}

TEST_F(PredictionExperimentTests, PreloadBeforeTheOpenReadsTheOpeningSegments) {
    BuildIndexes(WINDOW);

    Runtime::PredictionExperiment experiment({WINDOW}, 2);
    experiment.Preload(THURSDAY_OPEN_MS - 60LL * 60 * 1000);

    std::filesystem::remove_all("storage/indexes");
    std::filesystem::remove_all("storage/metadata");

    // The first forecast of the session is served from what was preloaded.
    const auto rows = experiment.RunSnapshot(MakeSnapshot(Days[0].second, 5000, 300));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(HasEveryForecast(rows[0]));
}

TEST_F(PredictionExperimentTests, LoadedIndexesAndForecastsAreReusedAcrossTradingDays) {
    constexpr std::size_t window = Core::DEFAULT_INPUT_WINDOW_SECONDS;
    constexpr std::size_t lastSecond = 5000;
    BuildIndexes(window);

    Runtime::LiveInputWindow::Snapshot snapshot{
        .Records = std::vector<Core::MLRecord>(
            Days[0].second.begin() + lastSecond + 1 - window,
            Days[0].second.begin() + lastSecond + 1
        ),
        .LastTimestampMs = THURSDAY_OPEN_MS + static_cast<std::int64_t>(lastSecond) * 1000
    };
    Runtime::PredictionExperiment experiment({window}, 3);
    experiment.Preload(snapshot.LastTimestampMs);

    // A later trading day must use the same loaded indexes, even when the files are gone.
    std::filesystem::remove_all("storage/indexes");
    std::filesystem::remove_all("storage/metadata");
    const auto thursday = experiment.RunSnapshot(snapshot);
    ASSERT_EQ(thursday.size(), 1u);
    ASSERT_TRUE(HasEveryForecast(thursday[0]));

    const Common::DateTime mondayOpen(2026, Common::SEP, 7, 9, 30, 0, true);
    snapshot.LastTimestampMs = (static_cast<std::int64_t>(mondayOpen.GetTimestamp()) + lastSecond) * 1000;
    const auto monday = experiment.RunSnapshot(snapshot);
    ASSERT_EQ(monday.size(), 1u);
    ASSERT_TRUE(HasEveryForecast(monday[0]));
    for (std::size_t i = 0; i < Runtime::PREDICTION_MATCH_COUNTS.size(); ++i) {
        EXPECT_EQ(monday[0][i]->AsOfTimestampMs, snapshot.LastTimestampMs);
        EXPECT_EQ(monday[0][i]->CumulativeChanges, thursday[0][i]->CumulativeChanges);
    }
}
