// sessionmultiindex_tests.cpp
// Tests for SessionMultiIndex (searching every half-hour segment of an input window as one) and the
// input-window selection settings in inputconfig.hpp.

#include "core/halfhourindex.hpp"
#include "core/halfhourmultiindex.hpp"
#include "core/inputconfig.hpp"
#include "core/sessionmultiindex.hpp"

#include <gtest/gtest.h>

#include <faiss/IndexFlat.h>
#include <faiss/index_io.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

    using Core::HalfHourIndex;
    using Core::SessionMultiIndex;

    constexpr std::size_t WINDOW = 10;

    std::array<char, 8> Day(const std::string& yyyymmdd) {
        std::array<char, 8> day{};
        std::copy_n(yyyymmdd.begin(), day.size(), day.begin());
        return day;
    }

    HalfHourIndex::QueryResult Candidate(const float distance, const std::string& yyyymmdd, const std::uint32_t recordIndex) {
        return HalfHourIndex::QueryResult{.distance = distance, .tradingDayId = Day(yyyymmdd), .recordIndex = recordIndex};
    }

    Core::NormalizationParameters MakeParameters(const double mean = 0.0, const double stddev = 1.0) {
        return Core::NormalizationParameters{.sample_count = 100, .mid_delta_mean = mean, .mid_delta_stddev = stddev};
    }

    /**
     * @brief One window in which every second moves by the same signed amount.
     * With the default parameters its magnitude vector is |value| repeated and its direction vector sign(value) repeated.
     */
    std::vector<Core::MLRecord> Flat(const float value) {
        return std::vector<Core::MLRecord>(WINDOW, Core::MLRecord{.mid_delta = value});
    }

    std::size_t SegmentOf(const HalfHourIndex::QueryResult& result) {
        return result.recordIndex / SessionMultiIndex::SECONDS_PER_SEGMENT;
    }

    /**
     * @brief An index holding every segment that exists on disk.
     */
    SessionMultiIndex LoadEverySegment() {
        SessionMultiIndex index(WINDOW);
        index.LoadSegments(SessionMultiIndex::SelectSegments(0, SessionMultiIndex::SEGMENT_COUNT));
        return index;
    }

    std::vector<std::size_t> Segments(const std::size_t first, const std::size_t last) {
        std::vector<std::size_t> segments;
        for (std::size_t segment = first; segment <= last; ++segment) segments.push_back(segment);
        return segments;
    }

}

/**
 * @brief Every test runs inside its own temporary working directory, since the indexes use relative
 * storage/ paths. Segments are built one window at a time so each test controls every distance.
 */
class SessionMultiIndexTests : public ::testing::Test {
protected:
    std::filesystem::path previousWorkingDirectory_;
    std::filesystem::path tempDirectory_;

    void SetUp() override {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        tempDirectory_ = std::filesystem::temp_directory_path() / ("sonny_session_index_tests_" + std::to_string(timestamp));

        std::filesystem::create_directories(tempDirectory_);
        previousWorkingDirectory_ = std::filesystem::current_path();
        std::filesystem::current_path(tempDirectory_);
    }

    void TearDown() override {
        std::filesystem::current_path(previousWorkingDirectory_);
        std::filesystem::remove_all(tempDirectory_);
    }

    /**
     * @brief A window stored in a segment: its records, the day it belongs to, and where it starts.
     */
    struct StoredWindow {
        std::vector<Core::MLRecord> records;
        std::string day = "2026-09-01";

        /**
         * @brief Record index of the window; defaults to the first second of its segment.
         */
        std::optional<std::size_t> recordIndex = std::nullopt;
    };

    /**
     * @brief Builds and saves (by destruction) one segment's magnitude and direction indexes.
     */
    static void BuildSegment(
        const std::size_t segment,
        const std::vector<StoredWindow>& windows,
        const Core::NormalizationParameters& parameters = MakeParameters()
    ) {
        const std::size_t segmentStart = segment * SessionMultiIndex::SECONDS_PER_SEGMENT;

        Core::HalfHourMultiIndex index(segmentStart, WINDOW);
        index.SetNormalizationParameters(parameters);

        for (const StoredWindow& window : windows) {
            std::vector<Core::MLRecord> records = window.records;
            index.AddVectorsToIndex(
                std::span<Core::MLRecord>(records),
                std::filesystem::path("storage/records") / (window.day + ".rec"),
                window.recordIndex.value_or(segmentStart)
            );
        }
    }

    /**
     * @brief Gives every segment one window that is far from Flat(1.0) in both sub-indexes.
     */
    static void BuildFarWindowInEverySegmentExcept(const std::vector<std::size_t>& skippedSegments) {
        for (std::size_t segment = 0; segment < SessionMultiIndex::SEGMENT_COUNT; ++segment) {
            if (std::find(skippedSegments.begin(), skippedSegments.end(), segment) == skippedSegments.end()) {
                BuildSegment(segment, {{.records = Flat(-5.0f)}});
            }
        }
    }
};

// ============================================================ //
// INPUT WINDOW SELECTION
// ============================================================ //

TEST(InputWindowSelectionTests, SingleWindowModeSelectsOnlyTheConfiguredWindow) {
    for (const std::size_t window : Core::INPUT_WINDOW_CONFIGURATIONS) {
        EXPECT_EQ(Core::SelectInputWindows(false, window), std::vector<std::size_t>{window});
    }
}

TEST(InputWindowSelectionTests, SingleWindowModeRejectsAnUnconfiguredWindow) {
    EXPECT_THROW(Core::SelectInputWindows(false, 25), std::invalid_argument);
}

TEST(InputWindowSelectionTests, AllWindowsModeSelectsEveryConfiguredWindowInOrder) {
    const std::vector<std::size_t> expected(Core::INPUT_WINDOW_CONFIGURATIONS.begin(), Core::INPUT_WINDOW_CONFIGURATIONS.end());

    // The selected window is ignored while every window runs.
    EXPECT_EQ(Core::SelectInputWindows(true, 60), expected);
    EXPECT_EQ(Core::SelectInputWindows(true, 10), expected);
}

TEST(InputWindowSelectionTests, ActiveWindowsFollowTheConfiguredSettings) {
    EXPECT_EQ(
        Core::ActiveInputWindows(),
        Core::SelectInputWindows(Core::USE_ALL_INPUT_WINDOWS, Core::SELECTED_INPUT_WINDOW_SECONDS)
    );
}

// ============================================================ //
// SEGMENT SELECTION
// ============================================================ //

TEST(SegmentSelectionTests, ChoosesTheSegmentsNearestTheTimeOfDay) {
    // Mid-session with room for five: two either side of the current segment.
    EXPECT_EQ(SessionMultiIndex::SelectSegments(6, 5), Segments(4, 8));

    // An even limit cannot be centred; the extra segment is the later one, which is needed next.
    EXPECT_EQ(SessionMultiIndex::SelectSegments(6, 4), Segments(5, 8));

    EXPECT_EQ(SessionMultiIndex::SelectSegments(6, 1), Segments(6, 6));
}

TEST(SegmentSelectionTests, LateInTheDayTakesTheLatterPartOfTheSession) {
    // The last hour is segments 11 (15:00) and 12 (15:30). With 8 allowed, both get 12:00 to the close.
    EXPECT_EQ(SessionMultiIndex::SelectSegments(11, 8), Segments(5, 12));
    EXPECT_EQ(SessionMultiIndex::SelectSegments(12, 8), Segments(5, 12));

    EXPECT_EQ(SessionMultiIndex::SelectSegments(12, 1), Segments(12, 12));
}

TEST(SegmentSelectionTests, EarlyInTheDayTakesTheStartOfTheSession) {
    EXPECT_EQ(SessionMultiIndex::SelectSegments(0, 8), Segments(0, 7));
    EXPECT_EQ(SessionMultiIndex::SelectSegments(1, 8), Segments(0, 7));
    EXPECT_EQ(SessionMultiIndex::SelectSegments(0, 1), Segments(0, 0));
}

TEST(SegmentSelectionTests, AlwaysReturnsAsManyConsecutiveSegmentsAsAllowed) {
    for (std::size_t limit = 1; limit <= SessionMultiIndex::SEGMENT_COUNT; ++limit) {
        for (std::size_t current = 0; current < SessionMultiIndex::SEGMENT_COUNT; ++current) {
            const auto segments = SessionMultiIndex::SelectSegments(current, limit);

            ASSERT_EQ(segments.size(), limit) << "current " << current;
            EXPECT_EQ(segments, Segments(segments.front(), segments.back()));
            EXPECT_LT(segments.back(), SessionMultiIndex::SEGMENT_COUNT);

            // The current segment is always among them.
            EXPECT_LE(segments.front(), current);
            EXPECT_GE(segments.back(), current);
        }
    }
}

TEST(SegmentSelectionTests, HandlesLimitsAndSegmentsOutsideTheSession) {
    // A limit of 13 or more is the whole session, whatever the time.
    EXPECT_EQ(SessionMultiIndex::SelectSegments(3, 13), Segments(0, 12));
    EXPECT_EQ(SessionMultiIndex::SelectSegments(3, 100), Segments(0, 12));

    // Past the close counts as the last segment.
    EXPECT_EQ(SessionMultiIndex::SelectSegments(40, 3), Segments(10, 12));

    EXPECT_TRUE(SessionMultiIndex::SelectSegments(5, 0).empty());
}

// ============================================================ //
// CANDIDATE SELECTION
// ============================================================ //

TEST(CandidateSelectionTests, KeepsTheNearestAcrossThePooledCandidates) {
    const auto selected = SessionMultiIndex::SelectNearest(
        {
            Candidate(5.0f, "20260901", 0),
            Candidate(1.0f, "20260901", 1800),
            Candidate(3.0f, "20260901", 3600),
            Candidate(2.0f, "20260901", 5400)
        },
        3
    );

    ASSERT_EQ(selected.size(), 3u);
    EXPECT_FLOAT_EQ(selected[0].distance, 1.0f);
    EXPECT_FLOAT_EQ(selected[1].distance, 2.0f);
    EXPECT_FLOAT_EQ(selected[2].distance, 3.0f);
}

TEST(CandidateSelectionTests, TiesPreferTheNewestDayThenTheEarliestRecord) {
    std::vector<HalfHourIndex::QueryResult> candidates{
        Candidate(4.0f, "20260902", 7200),
        Candidate(4.0f, "20260903", 9000),
        Candidate(4.0f, "20260903", 20),
        Candidate(4.0f, "20250101", 0),
        Candidate(3.0f, "20170301", 500)
    };

    const auto expectOrder = [](const std::vector<HalfHourIndex::QueryResult>& selected) {
        ASSERT_EQ(selected.size(), 5u);

        // A nearer match always wins, however old its day.
        EXPECT_EQ(selected[0].tradingDayId, Day("20170301"));

        EXPECT_EQ(selected[1].tradingDayId, Day("20260903"));
        EXPECT_EQ(selected[1].recordIndex, 20u);
        EXPECT_EQ(selected[2].tradingDayId, Day("20260903"));
        EXPECT_EQ(selected[2].recordIndex, 9000u);
        EXPECT_EQ(selected[3].tradingDayId, Day("20260902"));
        EXPECT_EQ(selected[4].tradingDayId, Day("20250101"));
    };

    expectOrder(SessionMultiIndex::SelectNearest(candidates, 10));

    // The order the segments were searched in must not change the outcome.
    std::reverse(candidates.begin(), candidates.end());
    expectOrder(SessionMultiIndex::SelectNearest(candidates, 10));

    std::rotate(candidates.begin(), candidates.begin() + 2, candidates.end());
    expectOrder(SessionMultiIndex::SelectNearest(candidates, 10));
}

TEST(CandidateSelectionTests, TiedCandidatesAtTheCapAreCutDeterministically) {
    const std::vector<HalfHourIndex::QueryResult> candidates{
        Candidate(0.0f, "20260901", 100),
        Candidate(0.0f, "20260904", 100),
        Candidate(0.0f, "20260902", 100),
        Candidate(0.0f, "20260903", 100)
    };

    const auto selected = SessionMultiIndex::SelectNearest(candidates, 2);

    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(selected[0].tradingDayId, Day("20260904"));
    EXPECT_EQ(selected[1].tradingDayId, Day("20260903"));
}

TEST(CandidateSelectionTests, DuplicateWindowIsKeptOnceWithItsSmallestDistance) {
    const auto selected = SessionMultiIndex::SelectNearest(
        {
            Candidate(2.0f, "20260901", 1795),
            Candidate(0.5f, "20260901", 1795),
            Candidate(1.0f, "20260902", 1795),   // Same record index on another day is a different window.
            Candidate(1.5f, "20260901", 1796)    // A neighbouring window is a different observation.
        },
        10
    );

    ASSERT_EQ(selected.size(), 3u);

    EXPECT_FLOAT_EQ(selected[0].distance, 0.5f);
    EXPECT_EQ(selected[0].tradingDayId, Day("20260901"));
    EXPECT_EQ(selected[0].recordIndex, 1795u);

    EXPECT_FLOAT_EQ(selected[1].distance, 1.0f);
    EXPECT_FLOAT_EQ(selected[2].distance, 1.5f);
}

TEST(CandidateSelectionTests, DuplicatesDoNotUseUpTheCap) {
    const auto selected = SessionMultiIndex::SelectNearest(
        {
            Candidate(0.0f, "20260901", 10),
            Candidate(0.0f, "20260901", 10),
            Candidate(0.1f, "20260901", 10),
            Candidate(1.0f, "20260901", 500),
            Candidate(2.0f, "20260901", 900)
        },
        2
    );

    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(selected[0].recordIndex, 10u);
    EXPECT_EQ(selected[1].recordIndex, 500u);
}

TEST(CandidateSelectionTests, DropsCandidatesWithoutAFiniteDistance) {
    const auto selected = SessionMultiIndex::SelectNearest(
        {
            Candidate(std::numeric_limits<float>::quiet_NaN(), "20260901", 1),
            Candidate(1.0f, "20260901", 2),
            Candidate(std::numeric_limits<float>::infinity(), "20260901", 3)
        },
        10
    );

    ASSERT_EQ(selected.size(), 1u);
    EXPECT_EQ(selected[0].recordIndex, 2u);
}

TEST(CandidateSelectionTests, HandlesEmptyInputAndAZeroCap) {
    EXPECT_TRUE(SessionMultiIndex::SelectNearest({}, 5).empty());
    EXPECT_TRUE(SessionMultiIndex::SelectNearest({Candidate(1.0f, "20260901", 0)}, 0).empty());
}

// ============================================================ //
// MAGNITUDE DISTANCE SCALE
// ============================================================ //

TEST(MagnitudeDistanceScaleTests, PooledStandardDeviationCombinesSpreadWithinAndBetweenSegments) {
    // Equal means: the pooled variance is the average of the segment variances.
    const std::vector<Core::NormalizationParameters> equalMeans{MakeParameters(0.5, 1.0), MakeParameters(0.5, 3.0)};
    EXPECT_NEAR(SessionMultiIndex::PooledStandardDeviation(equalMeans), std::sqrt(5.0), 1e-12);

    // No spread inside either segment, but their means differ: {0, 2} has a standard deviation of 1.
    const std::vector<Core::NormalizationParameters> differentMeans{MakeParameters(0.0, 0.0), MakeParameters(2.0, 0.0)};
    EXPECT_NEAR(SessionMultiIndex::PooledStandardDeviation(differentMeans), 1.0, 1e-12);

    // A single segment is its own reference, so its distances are left exactly as they are.
    const std::vector<Core::NormalizationParameters> single{MakeParameters(0.2, 0.7)};
    EXPECT_NEAR(SessionMultiIndex::PooledStandardDeviation(single), 0.7, 1e-12);
    EXPECT_NEAR(SessionMultiIndex::MagnitudeDistanceScale(0.7, SessionMultiIndex::PooledStandardDeviation(single)), 1.0, 1e-12);

    EXPECT_DOUBLE_EQ(SessionMultiIndex::PooledStandardDeviation({}), 0.0);
}

TEST(MagnitudeDistanceScaleTests, ScaleIsTheSquaredStandardDeviationRatio) {
    EXPECT_DOUBLE_EQ(SessionMultiIndex::MagnitudeDistanceScale(2.0, 4.0), 0.25);
    EXPECT_DOUBLE_EQ(SessionMultiIndex::MagnitudeDistanceScale(6.0, 2.0), 9.0);

    // Without a positive standard deviation the vectors carry no information to compare.
    EXPECT_DOUBLE_EQ(SessionMultiIndex::MagnitudeDistanceScale(0.0, 1.0), 0.0);
    EXPECT_DOUBLE_EQ(SessionMultiIndex::MagnitudeDistanceScale(1.0, 0.0), 0.0);
}

// ============================================================ //
// SEARCHING EVERY SEGMENT
// ============================================================ //

TEST_F(SessionMultiIndexTests, LoadsAndQueriesAllThirteenSegments) {
    // Segment s holds one window whose every second moves by 1 + s.
    for (std::size_t segment = 0; segment < SessionMultiIndex::SEGMENT_COUNT; ++segment) {
        BuildSegment(segment, {{.records = Flat(1.0f + static_cast<float>(segment))}});
    }

    const SessionMultiIndex index = LoadEverySegment();
    ASSERT_EQ(index.LoadedSegmentCount(), SessionMultiIndex::SEGMENT_COUNT);
    ASSERT_EQ(index.LoadedSegmentCount(), 13u);

    // Each segment's window can be found, so every segment was searched.
    for (std::size_t segment = 0; segment < SessionMultiIndex::SEGMENT_COUNT; ++segment) {
        const auto results = index.Query(Flat(1.0f + static_cast<float>(segment)), 1);

        ASSERT_EQ(results.MagnitudeResults.size(), 1u);
        EXPECT_NEAR(results.MagnitudeResults.front().distance, 0.0f, 1e-6f);
        EXPECT_EQ(results.MagnitudeResults.front().recordIndex, segment * SessionMultiIndex::SECONDS_PER_SEGMENT);
    }

    // With room for everything, each segment contributes its window to both sub-indexes.
    const auto everything = index.Query(Flat(1.0f), 32);
    ASSERT_EQ(everything.MagnitudeResults.size(), 13u);
    ASSERT_EQ(everything.DirectionResults.size(), 13u);

    for (std::size_t i = 0; i < everything.MagnitudeResults.size(); ++i) {
        // Magnitudes grow with the segment number, so the pooled order follows the segments.
        EXPECT_EQ(SegmentOf(everything.MagnitudeResults[i]), i);
    }
}

TEST_F(SessionMultiIndexTests, CapAppliesToThePooledCandidatesNotToEachSegment) {
    // Segment 3 holds the four nearest windows; every other segment holds one far window.
    // Days are added newest first, as Setup does, so a segment's own tie-break (lowest FAISS ID)
    // keeps the same newest days that the pooled ranking prefers.
    BuildSegment(3, {
        {.records = Flat(1.03f), .day = "2026-09-04"},
        {.records = Flat(1.02f), .day = "2026-09-03"},
        {.records = Flat(1.01f), .day = "2026-09-02"},
        {.records = Flat(1.00f), .day = "2026-09-01"}
    });
    BuildFarWindowInEverySegmentExcept({3});

    const SessionMultiIndex index = LoadEverySegment();
    ASSERT_EQ(index.LoadedSegmentCount(), 13u);

    const auto results = index.Query(Flat(1.0f), 3);

    // Three results in total, not three per segment, and all from the one segment that is nearest.
    ASSERT_EQ(results.MagnitudeResults.size(), 3u);
    ASSERT_EQ(results.DirectionResults.size(), 3u);

    EXPECT_EQ(results.MagnitudeResults[0].tradingDayId, Day("20260901"));
    EXPECT_EQ(results.MagnitudeResults[1].tradingDayId, Day("20260902"));
    EXPECT_EQ(results.MagnitudeResults[2].tradingDayId, Day("20260903"));

    for (const auto& result : results.MagnitudeResults) EXPECT_EQ(SegmentOf(result), 3u);
    for (const auto& result : results.DirectionResults) EXPECT_EQ(SegmentOf(result), 3u);

    // All four of segment 3's windows point up, so they tie in the direction index: newest days first.
    EXPECT_EQ(results.DirectionResults[0].tradingDayId, Day("20260904"));
    EXPECT_EQ(results.DirectionResults[1].tradingDayId, Day("20260903"));
    EXPECT_EQ(results.DirectionResults[2].tradingDayId, Day("20260902"));
}

TEST_F(SessionMultiIndexTests, RanksCandidatesFromDifferentSegmentsTogether) {
    BuildSegment(0, {{.records = Flat(1.3f)}});
    BuildSegment(6, {{.records = Flat(1.1f)}});
    BuildSegment(12, {{.records = Flat(1.2f)}});

    const SessionMultiIndex index = LoadEverySegment();
    const auto results = index.Query(Flat(1.0f), 2);

    ASSERT_EQ(results.MagnitudeResults.size(), 2u);
    EXPECT_EQ(SegmentOf(results.MagnitudeResults[0]), 6u);
    EXPECT_EQ(SegmentOf(results.MagnitudeResults[1]), 12u);
    EXPECT_LT(results.MagnitudeResults[0].distance, results.MagnitudeResults[1].distance);
}

TEST_F(SessionMultiIndexTests, MagnitudeAndDirectionAreRankedIndependently) {
    // Segment 2 matches the size of the query's moves but points the other way.
    BuildSegment(2, {{.records = Flat(-1.0f)}});

    // Segment 9 points the same way but its moves are four times larger.
    BuildSegment(9, {{.records = Flat(4.0f)}});

    const SessionMultiIndex index = LoadEverySegment();
    const auto results = index.Query(Flat(1.0f), 1);

    ASSERT_EQ(results.MagnitudeResults.size(), 1u);
    ASSERT_EQ(results.DirectionResults.size(), 1u);

    EXPECT_EQ(SegmentOf(results.MagnitudeResults.front()), 2u);
    EXPECT_NEAR(results.MagnitudeResults.front().distance, 0.0f, 1e-6f);

    EXPECT_EQ(SegmentOf(results.DirectionResults.front()), 9u);
    EXPECT_NEAR(results.DirectionResults.front().distance, 0.0f, 1e-6f);
}

TEST_F(SessionMultiIndexTests, MagnitudeDistancesAreComparedOnOneScaleAcrossSegments) {
    // Segment 1 (stddev 1) holds the window that is genuinely nearer: squared raw distance 10 × 0.2² = 0.4.
    // Segment 4 (stddev 10) holds a farther one: squared raw distance 10 × 0.5² = 2.5.
    const Core::NormalizationParameters quiet = MakeParameters(0.0, 1.0);
    const Core::NormalizationParameters volatileSegment = MakeParameters(0.0, 10.0);

    BuildSegment(1, {{.records = Flat(1.2f)}}, quiet);
    BuildSegment(4, {{.records = Flat(1.5f)}}, volatileSegment);

    // As reported by each segment, the volatile segment's distance is 100 times smaller than its raw
    // distance, so the farther window looks nearer. This is why raw distances cannot be pooled.
    {
        const Core::HalfHourMultiIndex quietIndex(1 * SessionMultiIndex::SECONDS_PER_SEGMENT, WINDOW);
        const Core::HalfHourMultiIndex volatileIndex(4 * SessionMultiIndex::SECONDS_PER_SEGMENT, WINDOW);

        const float quietDistance = quietIndex.Query(Flat(1.0f), 1).MagnitudeResults.front().distance;
        const float volatileDistance = volatileIndex.Query(Flat(1.0f), 1).MagnitudeResults.front().distance;

        EXPECT_NEAR(quietDistance, 0.4f, 1e-4f);
        EXPECT_NEAR(volatileDistance, 0.025f, 1e-5f);
        ASSERT_LT(volatileDistance, quietDistance);
    }

    const SessionMultiIndex index = LoadEverySegment();
    const auto results = index.Query(Flat(1.0f), 2);

    ASSERT_EQ(results.MagnitudeResults.size(), 2u);
    EXPECT_EQ(SegmentOf(results.MagnitudeResults[0]), 1u);
    EXPECT_EQ(SegmentOf(results.MagnitudeResults[1]), 4u);

    // On the common scale the two distances keep their raw proportion (2.5 / 0.4)...
    EXPECT_NEAR(results.MagnitudeResults[1].distance / results.MagnitudeResults[0].distance, 6.25f, 1e-3f);

    // ...and are expressed in units of the pooled standard deviation.
    const std::vector<Core::NormalizationParameters> parameters{quiet, volatileSegment};
    const double reference = SessionMultiIndex::PooledStandardDeviation(parameters);
    EXPECT_NEAR(results.MagnitudeResults[0].distance, 0.4 / (reference * reference), 1e-6);
    EXPECT_NEAR(results.MagnitudeResults[1].distance, 2.5 / (reference * reference), 1e-6);
}

TEST_F(SessionMultiIndexTests, SegmentNormalizationMeansDoNotAffectTheRanking) {
    // Same standard deviation, very different means: the mean cancels out of every distance.
    BuildSegment(0, {{.records = Flat(1.3f)}}, MakeParameters(0.0, 2.0));
    BuildSegment(7, {{.records = Flat(1.1f)}}, MakeParameters(50.0, 2.0));

    const SessionMultiIndex index = LoadEverySegment();
    const auto results = index.Query(Flat(1.0f), 2);

    ASSERT_EQ(results.MagnitudeResults.size(), 2u);
    EXPECT_EQ(SegmentOf(results.MagnitudeResults[0]), 7u);
    EXPECT_NEAR(results.MagnitudeResults[1].distance / results.MagnitudeResults[0].distance, 9.0f, 1e-2f);
}

TEST_F(SessionMultiIndexTests, SegmentWithoutAStandardDeviationIsLeftOutOfMagnitudeOnly) {
    // A zero standard deviation normalizes every vector to zeros, so its magnitude distances mean nothing.
    BuildSegment(0, {{.records = Flat(3.0f)}}, MakeParameters(0.0, 0.0));
    BuildSegment(1, {{.records = Flat(1.5f)}});

    const SessionMultiIndex index = LoadEverySegment();
    ASSERT_EQ(index.LoadedSegmentCount(), 2u);

    const auto results = index.Query(Flat(1.0f), 5);

    ASSERT_EQ(results.MagnitudeResults.size(), 1u);
    EXPECT_EQ(SegmentOf(results.MagnitudeResults.front()), 1u);

    // Its direction vectors are unaffected.
    EXPECT_EQ(results.DirectionResults.size(), 2u);
}

// ============================================================ //
// LOADING A SUBSET OF SEGMENTS
// ============================================================ //

TEST_F(SessionMultiIndexTests, NothingIsLoadedUntilSegmentsAreRequested) {
    BuildSegment(0, {{.records = Flat(1.0f)}});

    const SessionMultiIndex index(WINDOW);

    EXPECT_EQ(index.LoadedSegmentCount(), 0u);
    EXPECT_TRUE(index.Query(Flat(1.0f), 5).MagnitudeResults.empty());
    EXPECT_TRUE(index.Query(Flat(1.0f), 5).DirectionResults.empty());
}

TEST_F(SessionMultiIndexTests, LoadsAndSearchesOnlyTheRequestedSegments) {
    // Segment s holds one window whose every second moves by 1 + s.
    for (std::size_t segment = 0; segment < SessionMultiIndex::SEGMENT_COUNT; ++segment) {
        BuildSegment(segment, {{.records = Flat(1.0f + static_cast<float>(segment))}});
    }

    SessionMultiIndex index(WINDOW);
    index.LoadSegments(Segments(5, 8));

    EXPECT_EQ(index.LoadedSegments(), Segments(5, 8));
    EXPECT_EQ(index.LoadedSegmentCount(), 4u);

    const auto results = index.Query(Flat(1.0f), 32);

    // Segment 0 holds an exact match, but it is not loaded: the nearest loaded segment answers.
    ASSERT_EQ(results.MagnitudeResults.size(), 4u);
    ASSERT_EQ(results.DirectionResults.size(), 4u);
    EXPECT_EQ(SegmentOf(results.MagnitudeResults.front()), 5u);
    EXPECT_GT(results.MagnitudeResults.front().distance, 0.0f);

    for (const auto& result : results.MagnitudeResults) {
        EXPECT_GE(SegmentOf(result), 5u);
        EXPECT_LE(SegmentOf(result), 8u);
    }
}

TEST_F(SessionMultiIndexTests, MovingTheLoadedSegmentsReleasesTheOldAndLoadsTheNew) {
    for (std::size_t segment = 0; segment < SessionMultiIndex::SEGMENT_COUNT; ++segment) {
        BuildSegment(segment, {{.records = Flat(1.0f + static_cast<float>(segment))}});
    }

    SessionMultiIndex index(WINDOW);

    index.LoadSegments(Segments(0, 2));
    EXPECT_EQ(index.LoadedSegments(), Segments(0, 2));
    EXPECT_EQ(SegmentOf(index.Query(Flat(1.0f), 1).MagnitudeResults.front()), 0u);

    // One step later in the day: segment 0 is released and segment 3 loaded.
    index.LoadSegments(Segments(1, 3));
    EXPECT_EQ(index.LoadedSegments(), Segments(1, 3));
    EXPECT_EQ(SegmentOf(index.Query(Flat(1.0f), 1).MagnitudeResults.front()), 1u);
    EXPECT_NEAR(index.Query(Flat(4.0f), 1).MagnitudeResults.front().distance, 0.0f, 1e-6f);

    // The same request again changes nothing.
    index.LoadSegments(Segments(1, 3));
    EXPECT_EQ(index.LoadedSegments(), Segments(1, 3));

    // An empty request releases everything.
    index.LoadSegments({});
    EXPECT_EQ(index.LoadedSegmentCount(), 0u);
    EXPECT_TRUE(index.Query(Flat(1.0f), 5).MagnitudeResults.empty());
}

TEST_F(SessionMultiIndexTests, CommonMagnitudeScaleFollowsTheLoadedSegments) {
    BuildSegment(1, {{.records = Flat(1.2f)}}, MakeParameters(0.0, 1.0));
    BuildSegment(4, {{.records = Flat(1.5f)}}, MakeParameters(0.0, 10.0));

    SessionMultiIndex index(WINDOW);

    // Alone, a segment is its own reference: its distances are exactly what it reports itself.
    index.LoadSegments(Segments(4, 4));
    EXPECT_NEAR(index.Query(Flat(1.0f), 1).MagnitudeResults.front().distance, 0.025f, 1e-5f);

    // With a second segment loaded, both are re-expressed in units of their pooled standard deviation.
    index.LoadSegments(std::vector<std::size_t>{1, 4});

    const std::vector<Core::NormalizationParameters> parameters{MakeParameters(0.0, 1.0), MakeParameters(0.0, 10.0)};
    const double reference = SessionMultiIndex::PooledStandardDeviation(parameters);

    const auto results = index.Query(Flat(1.0f), 2);
    ASSERT_EQ(results.MagnitudeResults.size(), 2u);
    EXPECT_EQ(SegmentOf(results.MagnitudeResults[0]), 1u);
    EXPECT_NEAR(results.MagnitudeResults[1].distance, 2.5 / (reference * reference), 1e-6);
}

TEST_F(SessionMultiIndexTests, RejectsSegmentsOutsideTheSession) {
    SessionMultiIndex index(WINDOW);

    EXPECT_THROW(index.LoadSegments(std::vector<std::size_t>{SessionMultiIndex::SEGMENT_COUNT}), std::out_of_range);
    EXPECT_EQ(index.LoadedSegmentCount(), 0u);
}

// ============================================================ //
// DUPLICATES
// ============================================================ //

TEST_F(SessionMultiIndexTests, WindowStoredInTwoSegmentsIsReturnedOnceWithItsBestDistance) {
    // The same historical window (same day, same record index) filed under two segments.
    BuildSegment(0, {{.records = Flat(1.4f), .day = "2026-09-01", .recordIndex = 1795}});
    BuildSegment(1, {{.records = Flat(1.0f), .day = "2026-09-01", .recordIndex = 1795}});
    BuildSegment(2, {{.records = Flat(2.0f), .day = "2026-09-01"}});

    const SessionMultiIndex index = LoadEverySegment();
    const auto results = index.Query(Flat(1.0f), 10);

    for (const auto* streamResults : {&results.MagnitudeResults, &results.DirectionResults}) {
        ASSERT_EQ(streamResults->size(), 2u);

        const auto duplicates = std::count_if(streamResults->begin(), streamResults->end(), [](const auto& result) {
            return result.recordIndex == 1795u;
        });
        EXPECT_EQ(duplicates, 1);
    }

    // The exact copy (distance 0) is the one that is kept.
    EXPECT_EQ(results.MagnitudeResults.front().recordIndex, 1795u);
    EXPECT_NEAR(results.MagnitudeResults.front().distance, 0.0f, 1e-6f);
}

// ============================================================ //
// MISSING AND UNREADABLE SEGMENTS
// ============================================================ //

TEST_F(SessionMultiIndexTests, MissingSegmentsAreSkippedAndNeverCreated) {
    BuildSegment(0, {{.records = Flat(1.2f)}});
    BuildSegment(5, {{.records = Flat(1.1f)}});

    {
        const SessionMultiIndex index = LoadEverySegment();
        EXPECT_EQ(index.LoadedSegmentCount(), 2u);

        const auto results = index.Query(Flat(1.0f), 10);
        ASSERT_EQ(results.MagnitudeResults.size(), 2u);
        EXPECT_EQ(SegmentOf(results.MagnitudeResults[0]), 5u);
        EXPECT_EQ(SegmentOf(results.MagnitudeResults[1]), 0u);
        EXPECT_EQ(results.DirectionResults.size(), 2u);
    }

    // Constructing an index for a missing segment would save an empty one on destruction.
    EXPECT_FALSE(std::filesystem::exists("storage/indexes/10s/m/1000-1030.index"));
    EXPECT_FALSE(std::filesystem::exists("storage/indexes/10s/d/1530-1600.index"));
    EXPECT_FALSE(Core::HalfHourMultiIndex::FilesExist(1 * SessionMultiIndex::SECONDS_PER_SEGMENT, WINDOW));
}

TEST_F(SessionMultiIndexTests, SegmentMissingOneOfItsFilesIsSkippedWhole) {
    BuildSegment(0, {{.records = Flat(1.2f)}});
    BuildSegment(1, {{.records = Flat(1.1f)}});

    // Without its direction index, segment 1 contributes to neither sub-index.
    std::filesystem::remove("storage/indexes/10s/d/1000-1030.index");

    const SessionMultiIndex index = LoadEverySegment();
    EXPECT_EQ(index.LoadedSegmentCount(), 1u);

    const auto results = index.Query(Flat(1.0f), 10);
    ASSERT_EQ(results.MagnitudeResults.size(), 1u);
    EXPECT_EQ(SegmentOf(results.MagnitudeResults.front()), 0u);
    EXPECT_EQ(results.DirectionResults.size(), 1u);
}

TEST_F(SessionMultiIndexTests, SegmentThatFailsToLoadIsSkipped) {
    BuildSegment(0, {{.records = Flat(1.2f)}});
    BuildSegment(1, {{.records = Flat(1.1f)}});

    // Replace segment 1's magnitude index with one built for another input window.
    faiss::IndexFlatL2 incompatibleIndex(static_cast<int>(WINDOW) + 5);
    faiss::write_index(&incompatibleIndex, "storage/indexes/10s/m/1000-1030.index");

    const SessionMultiIndex index = LoadEverySegment();
    EXPECT_EQ(index.LoadedSegmentCount(), 1u);

    const auto results = index.Query(Flat(1.0f), 10);
    ASSERT_EQ(results.MagnitudeResults.size(), 1u);
    EXPECT_EQ(SegmentOf(results.MagnitudeResults.front()), 0u);
}

TEST_F(SessionMultiIndexTests, NoSegmentsMeansNoResultsAndNoFiles) {
    {
        const SessionMultiIndex index = LoadEverySegment();
        EXPECT_EQ(index.LoadedSegmentCount(), 0u);

        const auto results = index.Query(Flat(1.0f), 10);
        EXPECT_TRUE(results.MagnitudeResults.empty());
        EXPECT_TRUE(results.DirectionResults.empty());
    }

    EXPECT_FALSE(std::filesystem::exists("storage"));
}

TEST_F(SessionMultiIndexTests, QueryRejectsIncorrectRecordCount) {
    BuildSegment(0, {{.records = Flat(1.0f)}});

    const SessionMultiIndex index = LoadEverySegment();
    const std::vector<Core::MLRecord> tooShort(WINDOW - 1, Core::MLRecord{.mid_delta = 1.0f});

    EXPECT_THROW(index.Query(tooShort, 5), std::invalid_argument);
}
