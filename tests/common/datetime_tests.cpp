// datetime_tests.cpp
// Tests for the DateTime utility class.

#include <gtest/gtest.h>

#include "common/datetime.hpp"
#include <chrono>
#include <limits>

TEST(DateTime, ConstructsAndFormats)
{
    Common::DateTime dt(
        2026,
        Common::AUG,
        24,
        9,
        30,
        0,
        true
    );

    EXPECT_EQ(dt.GetYear(), 2026);
    EXPECT_EQ(static_cast<int>(dt.GetMonth()), 8);
    EXPECT_EQ(static_cast<int>(dt.GetDay()), 24);
    EXPECT_EQ(static_cast<int>(dt.GetHour()), 9);
    EXPECT_EQ(static_cast<int>(dt.GetMinute()), 30);
    EXPECT_EQ(static_cast<int>(dt.GetSecond()), 0);

    EXPECT_EQ(dt.ToString_DT(), "2026-08-24 09:30:00");
    EXPECT_EQ(dt.ToString_Date(), "2026-08-24");
    EXPECT_EQ(dt.ToString_Time(), "09:30:00");
}

TEST(DateTime, TimestampRoundTrip)
{
    Common::DateTime original(
        2026,
        Common::AUG,
        24,
        9,
        30,
        0,
        true
    );

    Common::DateTime recreated(
        original.GetTimestamp(),
        true
    );

    EXPECT_TRUE(original.Equal(recreated));
}

TEST(DateTime, NextDay)
{
    Common::DateTime dt(
        2026,
        Common::FEB,
        28,
        12,
        0,
        0,
        true
    );

    dt.NextDay();

    EXPECT_EQ(dt.GetYear(), 2026);
    EXPECT_EQ(static_cast<int>(dt.GetMonth()), 3);
    EXPECT_EQ(static_cast<int>(dt.GetDay()), 1);
}

TEST(DateTime, PreviousDay)
{
    Common::DateTime dt(
        2028,
        Common::MAR,
        1,
        12,
        0,
        0,
        true
    );

    // 2028 is a leap year, and the year boundary must roll back too.
    dt.PreviousDay();

    EXPECT_EQ(dt.GetYear(), 2028);
    EXPECT_EQ(static_cast<int>(dt.GetMonth()), 2);
    EXPECT_EQ(static_cast<int>(dt.GetDay()), 29);

    Common::DateTime newYear(2027, Common::JAN, 1, 12, 0, 0, true);
    newYear.PreviousDay();

    EXPECT_EQ(newYear.GetYear(), 2026);
    EXPECT_EQ(static_cast<int>(newYear.GetMonth()), 12);
    EXPECT_EQ(static_cast<int>(newYear.GetDay()), 31);
}

TEST(DateTime, Weekday)
{
    Common::DateTime monday(
        2026,
        Common::AUG,
        24,
        12,
        0,
        0,
        true
    );

    Common::DateTime saturday(
        2026,
        Common::AUG,
        22,
        12,
        0,
        0,
        true
    );

    EXPECT_TRUE(monday.IsWeekday());
    EXPECT_FALSE(saturday.IsWeekday());
}
TEST(DateTime, UtcFormattingDoesNotDependOnDisplayZone) {
    const Common::DateTime winter(2026, Common::NOV, 5, 9, 30, 0, true);
    const Common::DateTime summer(2026, Common::JUL, 6, 9, 30, 0, true);
    EXPECT_EQ(winter.ToString_UTC(), "2026-11-05T14:30:00Z");
    EXPECT_EQ(summer.ToString_UTC(), "2026-07-06T13:30:00Z");
    const Common::DateTime utc(winter.GetTimestamp(), Common::TimeZone::UTC);
    EXPECT_EQ(utc.ToString_Time(), "14:30:00");
    EXPECT_TRUE(winter.Equal(utc));
}

TEST(DateTime, SignedMillisecondsRoundTripAndFloorBeforeTheEpoch) {
    for (const std::int64_t timestamp : {-1001LL, -1000LL, -999LL, -1LL, 0LL, 1LL, 999LL, 1000LL, 1001LL}) {
        const auto date = Common::DateTime::FromTimestampMilliseconds(timestamp, Common::TimeZone::UTC);
        EXPECT_EQ(date.GetTimestampMilliseconds(), timestamp);
        const auto expectedSecond = timestamp / 1000 - (timestamp % 1000 < 0 ? 1 : 0);
        EXPECT_EQ(date.GetTimestamp(), expectedSecond);
    }
    auto date = Common::DateTime::FromTimestampMilliseconds(-1, Common::TimeZone::UTC);
    EXPECT_EQ(date.ToString_UTC(), "1969-12-31T23:59:59Z");
    EXPECT_EQ(date.GetYear(), 1969);
    date.SetTimestamp(-1);
    EXPECT_EQ(date.GetTimestampMilliseconds(), -1000);
}

TEST(DateTime, CalendarEditsRetainTheFractionalSecond) {
    auto date = Common::DateTime::FromTimestampMilliseconds(123, Common::TimeZone::UTC);
    date.SetHour(1);
    EXPECT_EQ(date.GetTimestampMilliseconds(), 3600123);
    date.NextDay();
    EXPECT_EQ(date.GetTimestampMilliseconds(), 90000123);
}

TEST(DateTime, NewYorkMillisecondsFollowBothDstTransitions) {
    const Common::DateTime spring(2026, Common::MAR, 8, 6, 59, 59, Common::TimeZone::UTC);
    const Common::DateTime autumn(2026, Common::NOV, 1, 5, 59, 59, Common::TimeZone::UTC);
    const auto springMs = spring.GetTimestampMilliseconds();
    const auto autumnMs = autumn.GetTimestampMilliseconds();
    EXPECT_EQ(Common::DateTime::FromTimestampMilliseconds(springMs + 999, Common::TimeZone::NewYork).ToString_Time(), "01:59:59");
    EXPECT_EQ(Common::DateTime::FromTimestampMilliseconds(springMs + 1000, Common::TimeZone::NewYork).ToString_Time(), "03:00:00");
    EXPECT_EQ(Common::DateTime::FromTimestampMilliseconds(autumnMs + 999, Common::TimeZone::NewYork).ToString_Time(), "01:59:59");
    EXPECT_EQ(Common::DateTime::FromTimestampMilliseconds(autumnMs + 1000, Common::TimeZone::NewYork).ToString_Time(), "01:00:00");
}

TEST(DateTime, CurrentMillisecondsAndCalendarTimeUseTheSameClock) {
    const auto before = Common::DateTime::GetCurrentTimestampMilliseconds();
    const auto current = Common::DateTime::GetCurrentDateTime(Common::TimeZone::UTC);
    const auto after = Common::DateTime::GetCurrentTimestampMilliseconds();
    EXPECT_GE(current.GetTimestampMilliseconds(), before);
    EXPECT_LE(current.GetTimestampMilliseconds(), after);
}

TEST(DateTime, ComparisonsIncludeMillisecondsAcrossTheEpoch) {
    const auto first = Common::DateTime::FromTimestampMilliseconds(-2, Common::TimeZone::UTC);
    const auto second = Common::DateTime::FromTimestampMilliseconds(-1, Common::TimeZone::UTC);
    const auto epoch = Common::DateTime::FromTimestampMilliseconds(0, Common::TimeZone::UTC);
    const auto sameInstant = Common::DateTime::FromTimestampMilliseconds(-1, Common::TimeZone::NewYork);
    EXPECT_FALSE(first.Equal(second));
    EXPECT_TRUE(first.Equal(second, true));
    EXPECT_LT(first, second);
    EXPECT_LT(second, epoch);
    EXPECT_GT(second, first);
    EXPECT_LE(second, sameInstant);
    EXPECT_GE(second, sameInstant);
    EXPECT_TRUE(second.Equal(sameInstant));
}

TEST(DateTime, RejectsTimestampsOutsideTheSupportedCalendarRange) {
    using namespace std::chrono;
    const auto firstMs = duration_cast<milliseconds>(sys_days{year{1}/January/1}.time_since_epoch()).count();
    const auto endMs = duration_cast<milliseconds>(
        (sys_days{year{32767}/December/31} + days{1}).time_since_epoch()
    ).count();
    const auto first = Common::DateTime::FromTimestampMilliseconds(firstMs, Common::TimeZone::UTC);
    const auto last = Common::DateTime::FromTimestampMilliseconds(endMs - 1, Common::TimeZone::UTC);
    EXPECT_EQ(first.ToString_Date(), "0001-01-01");
    EXPECT_EQ(last.GetYear(), 32767);
    EXPECT_EQ(last.GetTimestampMilliseconds(), endMs - 1);
    EXPECT_THROW(Common::DateTime::FromTimestampMilliseconds(firstMs - 1, Common::TimeZone::UTC), std::out_of_range);
    EXPECT_THROW(Common::DateTime::FromTimestampMilliseconds(endMs, Common::TimeZone::UTC), std::out_of_range);
    // A valid UTC instant can still lie outside the display zone's supported calendar.
    EXPECT_THROW(Common::DateTime::FromTimestampMilliseconds(firstMs, Common::TimeZone::NewYork), std::out_of_range);
    for (const auto extreme : {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
        EXPECT_THROW(Common::DateTime::FromTimestampMilliseconds(extreme, Common::TimeZone::UTC), std::out_of_range);
        EXPECT_THROW(Common::DateTime(extreme, Common::TimeZone::UTC), std::out_of_range);
    }
    const auto bceMs = duration_cast<milliseconds>(sys_days{year{-1}/January/1}.time_since_epoch()).count();
    EXPECT_THROW(Common::DateTime::FromTimestampMilliseconds(bceMs, Common::TimeZone::UTC), std::out_of_range);
}

TEST(DateTime, InvalidCalendarValuesAndFailedEditsDoNotCorruptState) {
    EXPECT_THROW(Common::DateTime(0, 1, 1, 0, 0, 0, Common::TimeZone::UTC), std::out_of_range);
    EXPECT_THROW(Common::DateTime(32768, 1, 1, 0, 0, 0, Common::TimeZone::UTC), std::out_of_range);
    EXPECT_THROW(Common::DateTime(2026, 2, 30, 0, 0, 0, Common::TimeZone::UTC), std::invalid_argument);
    auto date = Common::DateTime::FromTimestampMilliseconds(123, Common::TimeZone::UTC);
    EXPECT_THROW(date.SetTimestamp(std::numeric_limits<Common::UnixTimestamp>::max()), std::out_of_range);
    EXPECT_EQ(date.GetTimestampMilliseconds(), 123);
    EXPECT_THROW(date.SetYear(32768), std::out_of_range);
    EXPECT_EQ(date.GetTimestampMilliseconds(), 123);
    EXPECT_THROW(date.SetMonth(13), std::invalid_argument);
    EXPECT_THROW(date.SetDay(32), std::invalid_argument);
    EXPECT_THROW(date.SetHour(24), std::invalid_argument);
    EXPECT_THROW(date.SetMinute(60), std::invalid_argument);
    EXPECT_THROW(date.SetSecond(60), std::invalid_argument);
    EXPECT_EQ(date.ToString_DT(), "1970-01-01 00:00:00");
    EXPECT_EQ(date.GetTimestampMilliseconds(), 123);
}

TEST(DateTime, DaySteppingHandlesMonthBoundariesAndRejectsRangeOverflow) {
    Common::DateTime januaryEnd(2026, 1, 31, 12, 0, 0, Common::TimeZone::UTC);
    januaryEnd.NextDay();
    EXPECT_EQ(januaryEnd.ToString_Date(), "2026-02-01");
    januaryEnd.PreviousDay();
    EXPECT_EQ(januaryEnd.ToString_Date(), "2026-01-31");
    Common::DateTime first(1, 1, 1, 12, 0, 0, Common::TimeZone::UTC);
    Common::DateTime last(32767, 12, 31, 12, 0, 0, Common::TimeZone::UTC);
    EXPECT_THROW(first.PreviousDay(), std::out_of_range);
    EXPECT_THROW(last.NextDay(), std::out_of_range);
    EXPECT_EQ(first.ToString_Date(), "0001-01-01");
    EXPECT_EQ(last.ToString_Date(), "32767-12-31");
}
