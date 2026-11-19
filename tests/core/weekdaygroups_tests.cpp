// weekdaygroups_tests.cpp
// Tests for weekday-grouped direction indexes: the grouping configuration and its validation, which
// historical vectors each group's index receives, how the indexes are stored and validated, what the
// index builder selects, and which indexes a segment search uses.

#include "core/halfhourindex.hpp"
#include "core/halfhourmultiindex.hpp"
#include "core/sessionmultiindex.hpp"
#include "core/weekdayconfig.hpp"

#include <gtest/gtest.h>

#include <faiss/IndexFlat.h>
#include <faiss/index_io.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

    using Core::HalfHourIndex;
    using Core::HalfHourMultiIndex;
    using Core::WeekdayGroup;
    using SubIndexSelection = Core::HalfHourMultiIndex::SubIndexSelection;
    using namespace std::chrono;

    constexpr std::size_t WINDOW = 10;

    /**
     * @brief One trading week of New York session dates, Monday to Friday.
     */
    constexpr std::array<std::pair<const char*, weekday>, 5> TRADING_WEEK{{
        {"2026-08-31", Monday},
        {"2026-09-01", Tuesday},
        {"2026-09-02", Wednesday},
        {"2026-09-03", Thursday},
        {"2026-09-04", Friday},
    }};

    std::array<char, 8> DayId(const std::string& date) {
        std::array<char, 8> id{};
        std::size_t i = 0;
        for (const char c : date) {
            if (c != '-') id[i++] = c;
        }
        return id;
    }

    std::string GroupOf(const std::string& date, const std::span<const WeekdayGroup> groups = Core::DIRECTION_WEEKDAY_GROUPS) {
        const std::optional<WeekdayGroup> group = Core::FindWeekdayGroup(Core::TradingDayWeekday(DayId(date)), groups);
        return group.has_value() ? std::string(group->name) : std::string{};
    }

    /**
     * @brief The proposed Monday / TuesdayWednesday / ThursdayFriday regrouping.
     */
    const auto THREE_GROUPS = std::to_array<WeekdayGroup>({
        {"Monday", {Monday}},
        {"TuesdayWednesday", {Tuesday, Wednesday}},
        {"ThursdayFriday", {Thursday, Friday}},
    });

    /**
     * @brief Expects the groups to be rejected with a message containing the given text.
     */
    void ExpectRejected(const std::span<const WeekdayGroup> groups, const std::string& expectedText) {
        try {
            Core::ValidateWeekdayGroups(groups);
            ADD_FAILURE() << "Expected the groups to be rejected (" << expectedText << ")";
        }
        catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find(expectedText), std::string::npos) << error.what();
        }
    }

    struct StoredLocation {
        std::uint32_t recordIndex;
        std::array<char, 8> tradingDay;

        auto operator<=>(const StoredLocation&) const = default;
    };

    static_assert(sizeof(StoredLocation) == 12);

    /**
     * @brief Reads a .meta file: one (recordIndex, yyyymmdd) per FAISS vector, in vector-ID order.
     */
    std::vector<StoredLocation> ReadMetadata(const std::filesystem::path& path) {
        std::vector<StoredLocation> locations;
        std::ifstream file(path, std::ios::binary);
        StoredLocation location{};
        while (file.read(reinterpret_cast<char*>(&location), sizeof(location))) {
            locations.push_back(location);
        }
        return locations;
    }

    std::vector<std::vector<float>> ReadVectors(const std::filesystem::path& path) {
        std::unique_ptr<faiss::Index> index(faiss::read_index(path.string().c_str()));
        std::vector<std::vector<float>> vectors(static_cast<std::size_t>(index->ntotal));

        for (faiss::idx_t i = 0; i < index->ntotal; ++i) {
            vectors[static_cast<std::size_t>(i)].resize(static_cast<std::size_t>(index->d));
            index->reconstruct(i, vectors[static_cast<std::size_t>(i)].data());
        }

        return vectors;
    }

    std::vector<Core::MLRecord> MakeRecords(const std::size_t count, std::uint32_t seed) {
        std::vector<Core::MLRecord> records;
        records.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            seed = seed * 1103515245u + 12345u;
            const int direction = static_cast<int>((seed >> 16) % 3) - 1;
            const float size = 0.01f * static_cast<float>(1 + ((seed >> 8) % 50));
            records.push_back(Core::MLRecord{.mid_delta = static_cast<float>(direction) * size});
        }

        return records;
    }

    SubIndexSelection EverySubIndex(const std::span<const WeekdayGroup> groups = Core::DIRECTION_WEEKDAY_GROUPS) {
        return SubIndexSelection{.weekday_groups = {groups.begin(), groups.end()}};
    }

    const std::filesystem::path ALL_DAYS_DIRECTION_META = "storage/metadata/10s/d/0930-1000.meta";

    std::filesystem::path GroupIndexPath(const std::string& group) {
        return std::filesystem::path("storage/indexes/10s/d/weekdays") / group / "0930-1000.index";
    }

    std::filesystem::path GroupMetaPath(const std::string& group) {
        return std::filesystem::path("storage/metadata/10s/d/weekdays") / group / "0930-1000.meta";
    }

    std::filesystem::path GroupManifestPath(const std::string& group) {
        return std::filesystem::path("storage/metadata/10s/d/weekdays") / group / "0930-1000.weekdays.json";
    }

}

// ============================================================ //
// CONFIGURATION AND WEEKDAY LOOKUP
// ============================================================ //

TEST(WeekdayGroupConfigTests, InitialConfigurationIsFiveSingleDayGroupsInAllDaysMode) {
    EXPECT_EQ(Core::DIRECTION_SEARCH_MODE, Core::DirectionSearchMode::AllDays);
    ASSERT_EQ(Core::DIRECTION_WEEKDAY_GROUPS.size(), 5u);
    EXPECT_NO_THROW(Core::ValidateWeekdayGroups(Core::DIRECTION_WEEKDAY_GROUPS));
}

TEST(WeekdayGroupConfigTests, EachTradingDateResolvesToItsOwnWeekdayGroup) {
    // Explicit New York session dates, one per weekday.
    EXPECT_EQ(GroupOf("2026-08-31"), "Monday");
    EXPECT_EQ(GroupOf("2026-09-01"), "Tuesday");
    EXPECT_EQ(GroupOf("2026-09-02"), "Wednesday");
    EXPECT_EQ(GroupOf("2026-09-03"), "Thursday");
    EXPECT_EQ(GroupOf("2026-09-04"), "Friday");

    // Across a DST change and a year boundary.
    EXPECT_EQ(GroupOf("2026-03-09"), "Monday");
    EXPECT_EQ(GroupOf("2026-11-02"), "Monday");
    EXPECT_EQ(GroupOf("2027-01-01"), "Friday");
}

TEST(WeekdayGroupConfigTests, WeekendsBelongToNoGroup) {
    EXPECT_EQ(GroupOf("2026-09-05"), "");
    EXPECT_EQ(GroupOf("2026-09-06"), "");
}

TEST(WeekdayGroupConfigTests, RegroupingTuesdayAndWednesdayResolvesBothToTheSharedGroup) {
    EXPECT_NO_THROW(Core::ValidateWeekdayGroups(THREE_GROUPS));

    EXPECT_EQ(GroupOf("2026-08-31", THREE_GROUPS), "Monday");
    EXPECT_EQ(GroupOf("2026-09-01", THREE_GROUPS), "TuesdayWednesday");
    EXPECT_EQ(GroupOf("2026-09-02", THREE_GROUPS), "TuesdayWednesday");
    EXPECT_EQ(GroupOf("2026-09-03", THREE_GROUPS), "ThursdayFriday");
    EXPECT_EQ(GroupOf("2026-09-04", THREE_GROUPS), "ThursdayFriday");
}

TEST(WeekdayGroupConfigTests, TwoGroupSplitIsValid) {
    const auto groups = std::to_array<WeekdayGroup>({
        {"MondayTuesday", {Monday, Tuesday}},
        {"WednesdayThursdayFriday", {Wednesday, Thursday, Friday}},
    });

    EXPECT_NO_THROW(Core::ValidateWeekdayGroups(groups));
    EXPECT_EQ(GroupOf("2026-09-01", groups), "MondayTuesday");
    EXPECT_EQ(GroupOf("2026-09-04", groups), "WednesdayThursdayFriday");
}

TEST(WeekdayGroupConfigTests, RejectsInvalidOrAmbiguousGroupings) {
    ExpectRejected(std::span<const WeekdayGroup>{}, "At least one");

    ExpectRejected(std::to_array<WeekdayGroup>({
        {"Monday", {Monday}},
        {"TuesdayWednesday", {Tuesday, Wednesday}},
        {"WednesdayThursdayFriday", {Wednesday, Thursday, Friday}},
    }), "Wednesday is in both weekday groups \"TuesdayWednesday\" and \"WednesdayThursdayFriday\"");

    ExpectRejected(std::to_array<WeekdayGroup>({
        {"Monday", {Monday, Monday}},
        {"Rest", {Tuesday, Wednesday, Thursday, Friday}},
    }), "lists Monday more than once");

    ExpectRejected(std::to_array<WeekdayGroup>({
        {"Monday", {Monday}},
        {"TuesdayWednesday", {Tuesday, Wednesday}},
        {"Thursday", {Thursday}},
    }), "Friday is not in any weekday group");

    ExpectRejected(std::to_array<WeekdayGroup>({
        {"Weekend", {Saturday}},
        {"Week", {Monday, Tuesday, Wednesday, Thursday, Friday}},
    }), "Saturday, which is not a trading weekday");

    ExpectRejected(std::to_array<WeekdayGroup>({
        {"Empty", {}},
        {"Week", {Monday, Tuesday, Wednesday, Thursday, Friday}},
    }), "has no weekdays");

    ExpectRejected(std::to_array<WeekdayGroup>({
        {"Days", {Monday, Tuesday}},
        {"Days", {Wednesday, Thursday, Friday}},
    }), "configured more than once");

    ExpectRejected(std::to_array<WeekdayGroup>({
        {"Mon/Tue", {Monday, Tuesday}},
        {"Rest", {Wednesday, Thursday, Friday}},
    }), "must be non-empty and use only letters");

    ExpectRejected(std::to_array<WeekdayGroup>({
        {"AllDays", {Monday, Tuesday, Wednesday, Thursday, Friday}},
    }), "reserved");
}

TEST(WeekdayGroupConfigTests, TradingDayIdsMustBeValidDates) {
    EXPECT_EQ(Core::TradingDayWeekday(DayId("2026-09-01")), Tuesday);
    EXPECT_THROW(Core::TradingDayWeekday(DayId("2026-13-01")), std::invalid_argument);
    EXPECT_THROW(Core::TradingDayWeekday(std::array<char, 8>{'2', '0', '2', '6', '-', '0', '9', '1'}), std::invalid_argument);
}

TEST(WeekdayGroupConfigTests, SearchModesSelectTheirDirectionIndexes) {
    using enum Core::DirectionSearchMode;

    EXPECT_TRUE(Core::SearchesAllDaysDirection(AllDays));
    EXPECT_FALSE(Core::SearchesWeekdayDirection(AllDays));
    EXPECT_FALSE(Core::SearchesAllDaysDirection(WeekdayGroups));
    EXPECT_TRUE(Core::SearchesWeekdayDirection(WeekdayGroups));
    EXPECT_TRUE(Core::SearchesAllDaysDirection(Compare));
    EXPECT_TRUE(Core::SearchesWeekdayDirection(Compare));

    const WeekdayGroup tuesday = Core::DIRECTION_WEEKDAY_GROUPS[1];

    const SubIndexSelection allDays = SubIndexSelection::ForSearch(AllDays, tuesday);
    EXPECT_TRUE(allDays.magnitude && allDays.all_days_direction && allDays.weekday_groups.empty());

    const SubIndexSelection weekday = SubIndexSelection::ForSearch(WeekdayGroups, tuesday);
    EXPECT_TRUE(weekday.magnitude);
    EXPECT_FALSE(weekday.all_days_direction);
    ASSERT_EQ(weekday.weekday_groups.size(), 1u);
    EXPECT_EQ(weekday.weekday_groups[0].name, "Tuesday");

    const SubIndexSelection compare = SubIndexSelection::ForSearch(Compare, tuesday);
    EXPECT_TRUE(compare.magnitude && compare.all_days_direction);
    ASSERT_EQ(compare.weekday_groups.size(), 1u);

    EXPECT_THROW(SubIndexSelection::ForSearch(WeekdayGroups, std::nullopt), std::invalid_argument);
}

// ============================================================ //
// HISTORICAL INDEX ASSIGNMENT AND PERSISTENCE
// ============================================================ //

/**
 * @brief Runs inside a temporary working directory, since the indexes use relative storage/ paths.
 */
class WeekdayIndexTests : public ::testing::Test {
protected:
    std::filesystem::path previousWorkingDirectory_;
    std::filesystem::path tempDirectory_;

    void SetUp() override {
        const auto timestamp = steady_clock::now().time_since_epoch().count();
        tempDirectory_ = std::filesystem::temp_directory_path() / ("sonny_weekday_index_tests_" + std::to_string(timestamp));

        std::filesystem::create_directories(tempDirectory_);
        previousWorkingDirectory_ = std::filesystem::current_path();
        std::filesystem::current_path(tempDirectory_);
    }

    void TearDown() override {
        std::filesystem::current_path(previousWorkingDirectory_);
        std::filesystem::remove_all(tempDirectory_);
    }

    /**
     * @brief Builds segment 0 from one week of days, newest first, as Setup does.
     */
    static void BuildWeek(const SubIndexSelection& subIndexes, const std::size_t recordsPerDay = WINDOW + 4) {
        HalfHourMultiIndex index(0, WINDOW, subIndexes);

        if (subIndexes.magnitude) {
            index.SetNormalizationParameters(Core::NormalizationParameters{.sample_count = 100, .mid_delta_mean = 0.0, .mid_delta_stddev = 1.0});
        }

        for (std::size_t day = TRADING_WEEK.size(); day-- > 0;) {
            std::vector<Core::MLRecord> records = MakeRecords(recordsPerDay, static_cast<std::uint32_t>(day + 1));
            index.AddVectorsToIndex(
                std::span<Core::MLRecord>(records),
                std::filesystem::path("storage/records") / (std::string(TRADING_WEEK[day].first) + ".rec"),
                0
            );
        }
    }
};

TEST_F(WeekdayIndexTests, WeekdayIndexesAreStoredApartFromTheAllWeekIndex) {
    BuildWeek(EverySubIndex());

    // The all-week files keep their existing paths.
    EXPECT_TRUE(std::filesystem::exists("storage/indexes/10s/d/0930-1000.index"));
    EXPECT_TRUE(std::filesystem::exists("storage/indexes/10s/m/0930-1000.index"));
    EXPECT_TRUE(std::filesystem::exists("storage/metadata/10s/m/0930-1000.normalization.json"));

    for (const WeekdayGroup& group : Core::DIRECTION_WEEKDAY_GROUPS) {
        const std::string name(group.name);
        EXPECT_TRUE(std::filesystem::exists(GroupIndexPath(name))) << name;
        EXPECT_TRUE(std::filesystem::exists(GroupMetaPath(name))) << name;
        EXPECT_TRUE(std::filesystem::exists(GroupManifestPath(name))) << name;
        EXPECT_FALSE(std::filesystem::exists(GroupMetaPath(name).replace_extension(".normalization.json"))) << name;
    }

    // Magnitude is never split by weekday.
    EXPECT_FALSE(std::filesystem::exists("storage/indexes/10s/m/weekdays"));
    EXPECT_FALSE(std::filesystem::exists("storage/metadata/10s/m/weekdays"));
}

TEST_F(WeekdayIndexTests, EachGroupHoldsOnlyItsOwnWeekdaysAndEverySampleExactlyOnce) {
    BuildWeek(EverySubIndex());

    const std::vector<StoredLocation> allDays = ReadMetadata(ALL_DAYS_DIRECTION_META);

    // The all-week baseline still holds every trading weekday.
    std::set<std::array<char, 8>> allDaysTradingDays;
    for (const StoredLocation& location : allDays) allDaysTradingDays.insert(location.tradingDay);
    EXPECT_EQ(allDaysTradingDays.size(), TRADING_WEEK.size());

    std::multiset<StoredLocation> partitioned;

    for (const WeekdayGroup& group : Core::DIRECTION_WEEKDAY_GROUPS) {
        const std::vector<StoredLocation> locations = ReadMetadata(GroupMetaPath(std::string(group.name)));
        ASSERT_FALSE(locations.empty()) << group.name;

        for (const StoredLocation& location : locations) {
            EXPECT_TRUE(group.Contains(Core::TradingDayWeekday(location.tradingDay)))
                << std::string(location.tradingDay.data(), 8) << " in " << group.name;
            partitioned.insert(location);
        }
    }

    // The groups partition the all-week samples: same samples, each once.
    EXPECT_EQ(partitioned, std::multiset<StoredLocation>(allDays.begin(), allDays.end()));
    EXPECT_EQ(std::set<StoredLocation>(partitioned.begin(), partitioned.end()).size(), partitioned.size());
}

TEST_F(WeekdayIndexTests, MondayAndTuesdayVectorsEnterOnlyTheirOwnGroups) {
    BuildWeek(EverySubIndex());

    for (const StoredLocation& location : ReadMetadata(GroupMetaPath("Monday"))) {
        EXPECT_EQ(location.tradingDay, DayId("2026-08-31"));
    }

    for (const StoredLocation& location : ReadMetadata(GroupMetaPath("Tuesday"))) {
        EXPECT_EQ(location.tradingDay, DayId("2026-09-01"));
    }
}

TEST_F(WeekdayIndexTests, WeekdayVectorsAreIdenticalToTheirAllWeekVectors) {
    // Same construction and temporal weighting: only membership differs.
    BuildWeek(EverySubIndex());

    const std::vector<StoredLocation> allDaysLocations = ReadMetadata(ALL_DAYS_DIRECTION_META);
    const std::vector<std::vector<float>> allDaysVectors = ReadVectors("storage/indexes/10s/d/0930-1000.index");

    std::map<StoredLocation, std::vector<float>> allDaysByLocation;
    for (std::size_t i = 0; i < allDaysLocations.size(); ++i) {
        allDaysByLocation[allDaysLocations[i]] = allDaysVectors[i];
    }

    for (const WeekdayGroup& group : Core::DIRECTION_WEEKDAY_GROUPS) {
        const std::string name(group.name);
        const std::vector<StoredLocation> locations = ReadMetadata(GroupMetaPath(name));
        const std::vector<std::vector<float>> vectors = ReadVectors(GroupIndexPath(name));
        ASSERT_EQ(locations.size(), vectors.size()) << name;

        for (std::size_t i = 0; i < vectors.size(); ++i) {
            EXPECT_EQ(vectors[i], allDaysByLocation.at(locations[i])) << name << " vector " << i;
        }
    }
}

TEST_F(WeekdayIndexTests, MultiDayGroupsCollectEveryDayInTheGroup) {
    BuildWeek(SubIndexSelection{.magnitude = false, .all_days_direction = false, .weekday_groups = {THREE_GROUPS.begin(), THREE_GROUPS.end()}});

    std::set<std::array<char, 8>> days;
    for (const StoredLocation& location : ReadMetadata(GroupMetaPath("TuesdayWednesday"))) days.insert(location.tradingDay);

    EXPECT_EQ(days, (std::set<std::array<char, 8>>{DayId("2026-09-01"), DayId("2026-09-02")}));

    // Only the selected sub-indexes were written.
    EXPECT_FALSE(std::filesystem::exists("storage/indexes/10s/m"));
    EXPECT_FALSE(std::filesystem::exists("storage/indexes/10s/d/0930-1000.index"));
}

TEST_F(WeekdayIndexTests, ManifestMustMatchTheConfiguredGroup) {
    BuildWeek(EverySubIndex());

    const WeekdayGroup tuesday = Core::DIRECTION_WEEKDAY_GROUPS[1];
    EXPECT_TRUE(HalfHourIndex::FilesExist(0, WINDOW, HalfHourIndex::INX_TYPE_DIRECTION, tuesday));

    // A later grouping that reuses the name for different weekdays must not load the old index.
    const WeekdayGroup regroupedTuesday{"Tuesday", {Tuesday, Wednesday}};
    EXPECT_FALSE(HalfHourIndex::FilesExist(0, WINDOW, HalfHourIndex::INX_TYPE_DIRECTION, regroupedTuesday));
    EXPECT_THROW(
        HalfHourMultiIndex(0, WINDOW, SubIndexSelection{.magnitude = false, .all_days_direction = false, .weekday_groups = {regroupedTuesday}}),
        std::runtime_error
    );

    // An index from another input window is also rejected.
    std::filesystem::create_directories("storage/indexes/20s/d/weekdays/Tuesday");
    std::filesystem::create_directories("storage/metadata/20s/d/weekdays/Tuesday");
    std::filesystem::copy_file(GroupIndexPath("Tuesday"), "storage/indexes/20s/d/weekdays/Tuesday/0930-1000.index");
    std::filesystem::copy_file(GroupMetaPath("Tuesday"), "storage/metadata/20s/d/weekdays/Tuesday/0930-1000.meta");
    std::filesystem::copy_file(GroupManifestPath("Tuesday"), "storage/metadata/20s/d/weekdays/Tuesday/0930-1000.weekdays.json");
    EXPECT_FALSE(HalfHourIndex::FilesExist(0, 20, HalfHourIndex::INX_TYPE_DIRECTION, tuesday));

    // Without its manifest a weekday index is incomplete.
    std::filesystem::remove(GroupManifestPath("Tuesday"));
    EXPECT_FALSE(HalfHourIndex::FilesExist(0, WINDOW, HalfHourIndex::INX_TYPE_DIRECTION, tuesday));
}

TEST_F(WeekdayIndexTests, AllWeekAndWeekdayIndexesNeverStandInForEachOther) {
    // Only the all-week indexes exist.
    BuildWeek(SubIndexSelection{});

    const WeekdayGroup monday = Core::DIRECTION_WEEKDAY_GROUPS[0];
    EXPECT_TRUE(HalfHourMultiIndex::FilesExist(0, WINDOW));
    EXPECT_FALSE(HalfHourIndex::FilesExist(0, WINDOW, HalfHourIndex::INX_TYPE_DIRECTION, monday));
    EXPECT_FALSE(HalfHourMultiIndex::FilesExist(0, WINDOW, SubIndexSelection::ForSearch(Core::DirectionSearchMode::WeekdayGroups, monday)));

    // Only weekday indexes exist (plus magnitude).
    std::filesystem::remove_all("storage");
    BuildWeek(SubIndexSelection{.all_days_direction = false, .weekday_groups = {monday}});

    EXPECT_FALSE(HalfHourMultiIndex::FilesExist(0, WINDOW));
    EXPECT_TRUE(HalfHourMultiIndex::FilesExist(0, WINDOW, SubIndexSelection::ForSearch(Core::DirectionSearchMode::WeekdayGroups, monday)));
}

// ============================================================ //
// INDEX BUILD SELECTION
// ============================================================ //

TEST_F(WeekdayIndexTests, BuildSelectionFollowsTheSearchMode) {
    using enum Core::DirectionSearchMode;
    const auto groups = Core::DIRECTION_WEEKDAY_GROUPS;

    const SubIndexSelection allDays = SubIndexSelection::ToBuild(0, WINDOW, AllDays, groups, true);
    EXPECT_TRUE(allDays.magnitude && allDays.all_days_direction);
    EXPECT_TRUE(allDays.weekday_groups.empty());

    const SubIndexSelection weekday = SubIndexSelection::ToBuild(0, WINDOW, Core::DirectionSearchMode::WeekdayGroups, groups, true);
    EXPECT_TRUE(weekday.magnitude);
    EXPECT_FALSE(weekday.all_days_direction);
    EXPECT_EQ(weekday.weekday_groups.size(), groups.size());

    const SubIndexSelection compare = SubIndexSelection::ToBuild(0, WINDOW, Compare, groups, true);
    EXPECT_TRUE(compare.magnitude && compare.all_days_direction);
    EXPECT_EQ(compare.weekday_groups.size(), groups.size());

    // Configured groups only.
    const SubIndexSelection regrouped = SubIndexSelection::ToBuild(0, WINDOW, Compare, THREE_GROUPS, true);
    ASSERT_EQ(regrouped.weekday_groups.size(), 3u);
    EXPECT_EQ(regrouped.weekday_groups[1].name, "TuesdayWednesday");
}

TEST_F(WeekdayIndexTests, BuildSelectionReusesCompleteAllWeekIndexes) {
    using enum Core::DirectionSearchMode;

    // Existing all-week magnitude and direction indexes for 10s only.
    BuildWeek(SubIndexSelection{});

    const SubIndexSelection reuse = SubIndexSelection::ToBuild(0, WINDOW, Compare, Core::DIRECTION_WEEKDAY_GROUPS, true);
    EXPECT_FALSE(reuse.magnitude);
    EXPECT_FALSE(reuse.all_days_direction);
    EXPECT_EQ(reuse.weekday_groups.size(), Core::DIRECTION_WEEKDAY_GROUPS.size());

    // Reuse switched off: everything the mode needs is rebuilt.
    const SubIndexSelection rebuild = SubIndexSelection::ToBuild(0, WINDOW, Compare, Core::DIRECTION_WEEKDAY_GROUPS, false);
    EXPECT_TRUE(rebuild.magnitude && rebuild.all_days_direction);
    EXPECT_EQ(rebuild.weekday_groups.size(), Core::DIRECTION_WEEKDAY_GROUPS.size());

    // Another input window has nothing to reuse.
    const SubIndexSelection otherWindow = SubIndexSelection::ToBuild(0, 20, AllDays, Core::DIRECTION_WEEKDAY_GROUPS, true);
    EXPECT_TRUE(otherWindow.magnitude && otherWindow.all_days_direction);

    // Once the weekday indexes exist too, nothing is left to build.
    BuildWeek(SubIndexSelection{.magnitude = false, .all_days_direction = false, .weekday_groups = reuse.weekday_groups});
    EXPECT_TRUE(SubIndexSelection::ToBuild(0, WINDOW, Compare, Core::DIRECTION_WEEKDAY_GROUPS, true).Empty());
}

TEST_F(WeekdayIndexTests, RegroupingRebuildsOnlyGroupsThatChanged) {
    BuildWeek(EverySubIndex());

    const SubIndexSelection regrouped = SubIndexSelection::ToBuild(0, WINDOW, Core::DirectionSearchMode::WeekdayGroups, THREE_GROUPS, true);
    EXPECT_FALSE(regrouped.magnitude);
    ASSERT_EQ(regrouped.weekday_groups.size(), 2u);
    EXPECT_EQ(regrouped.weekday_groups[0].name, "TuesdayWednesday");
    EXPECT_EQ(regrouped.weekday_groups[1].name, "ThursdayFriday");

    // A reused name with new weekdays is rebuilt as well.
    const auto renamed = std::to_array<WeekdayGroup>({
        {"Monday", {Monday}},
        {"Tuesday", {Tuesday, Wednesday}},
        {"ThursdayFriday", {Thursday, Friday}},
    });
    const SubIndexSelection renamedBuild = SubIndexSelection::ToBuild(0, WINDOW, Core::DirectionSearchMode::WeekdayGroups, renamed, true);
    ASSERT_EQ(renamedBuild.weekday_groups.size(), 2u);
    EXPECT_EQ(renamedBuild.weekday_groups[0].name, "Tuesday");
}

TEST_F(WeekdayIndexTests, DeleteFilesRemovesOnlyTheSelectedSubIndexes) {
    BuildWeek(EverySubIndex());

    const WeekdayGroup tuesday = Core::DIRECTION_WEEKDAY_GROUPS[1];
    HalfHourMultiIndex::DeleteFiles(0, WINDOW, SubIndexSelection{.magnitude = false, .all_days_direction = false, .weekday_groups = {tuesday}});

    EXPECT_FALSE(std::filesystem::exists(GroupIndexPath("Tuesday")));
    EXPECT_FALSE(std::filesystem::exists(GroupManifestPath("Tuesday")));
    EXPECT_TRUE(std::filesystem::exists(GroupIndexPath("Monday")));
    EXPECT_TRUE(HalfHourMultiIndex::FilesExist(0, WINDOW));
}

// ============================================================ //
// SEGMENT SEARCH
// ============================================================ //

TEST_F(WeekdayIndexTests, WeekdaySearchReturnsOnlyThatGroupsMatchesAndLeavesTheBaselineUnchanged) {
    BuildWeek(EverySubIndex(), 200);

    // Query with a Tuesday window, searched as on a Monday and as on a Tuesday.
    const std::vector<Core::MLRecord> tuesdayRecords = MakeRecords(200, 2);
    const std::span<const Core::MLRecord> query = std::span<const Core::MLRecord>(tuesdayRecords).subspan(50, WINDOW);
    const std::vector<std::size_t> segments{0};

    Core::SessionMultiIndex baseline(WINDOW);
    baseline.LoadSegments(segments);
    const auto baselineResults = baseline.Query(query, 64);

    for (const std::size_t groupIndex : {std::size_t{0}, std::size_t{1}}) {
        const WeekdayGroup group = Core::DIRECTION_WEEKDAY_GROUPS[groupIndex];

        Core::SessionMultiIndex compare(WINDOW, SubIndexSelection::ForSearch(Core::DirectionSearchMode::Compare, group));
        compare.LoadSegments(segments);
        const auto results = compare.Query(query, 64);

        ASSERT_FALSE(results.WeekdayDirectionResults.empty());
        for (const HalfHourIndex::QueryResult& result : results.WeekdayDirectionResults) {
            EXPECT_TRUE(group.Contains(Core::TradingDayWeekday(result.tradingDayId))) << group.name;
        }

        // The all-week results in Compare mode are exactly those of the AllDays-only search,
        // and they span more than one weekday.
        ASSERT_EQ(results.DirectionResults.size(), baselineResults.DirectionResults.size());
        for (std::size_t i = 0; i < results.DirectionResults.size(); ++i) {
            EXPECT_EQ(results.DirectionResults[i].tradingDayId, baselineResults.DirectionResults[i].tradingDayId);
            EXPECT_EQ(results.DirectionResults[i].recordIndex, baselineResults.DirectionResults[i].recordIndex);
            EXPECT_EQ(results.DirectionResults[i].distance, baselineResults.DirectionResults[i].distance);
        }

        std::set<std::array<char, 8>> baselineDays;
        for (const auto& result : results.DirectionResults) baselineDays.insert(result.tradingDayId);
        EXPECT_GT(baselineDays.size(), 1u);
    }

    // Searched as on a Tuesday, the query's own window is an exact weekday match.
    Core::SessionMultiIndex tuesday(WINDOW, SubIndexSelection::ForSearch(Core::DirectionSearchMode::WeekdayGroups, Core::DIRECTION_WEEKDAY_GROUPS[1]));
    tuesday.LoadSegments(segments);
    const auto tuesdayResults = tuesday.Query(query, 1);
    EXPECT_TRUE(tuesdayResults.DirectionResults.empty());
    ASSERT_EQ(tuesdayResults.WeekdayDirectionResults.size(), 1u);
    EXPECT_EQ(tuesdayResults.WeekdayDirectionResults[0].distance, 0.0f);
}

TEST_F(WeekdayIndexTests, MissingWeekdayIndexesKeepTheSegmentOutOfBothSearches) {
    // Compare mode searches the same segments for both streams, so a segment without its weekday
    // index is skipped whole rather than searched by the baseline alone.
    BuildWeek(SubIndexSelection{});

    Core::SessionMultiIndex compare(WINDOW, SubIndexSelection::ForSearch(Core::DirectionSearchMode::Compare, Core::DIRECTION_WEEKDAY_GROUPS[0]));
    compare.LoadSegments(std::vector<std::size_t>{0});

    EXPECT_EQ(compare.LoadedSegmentCount(), 0u);
    EXPECT_FALSE(std::filesystem::exists("storage/indexes/10s/d/weekdays"));
}
