// datetime_tests.cpp
// Tests for the DateTime utility class.

#include <gtest/gtest.h>

#include "common/datetime.hpp"

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