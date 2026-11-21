#include "types/date.h"

#include <gtest/gtest.h>

namespace quarry {
namespace {

int32_t Day(const char* text) { return ParseDate(text).ValueOrDie(); }

TEST(DateTest, ParsesToDaysSinceTheEpoch) {
  EXPECT_EQ(Day("1970-01-01"), 0);
  EXPECT_EQ(Day("1970-01-02"), 1);
  EXPECT_EQ(Day("1969-12-31"), -1);
  EXPECT_EQ(Day("2000-03-01"), 11017);
  EXPECT_EQ(Day("1998-12-01"), 10561);
}

TEST(DateTest, DayArithmeticIsPlainSubtraction) {
  // TPC-H Q1: date '1998-12-01' - interval '90' day.
  EXPECT_EQ(Day("1998-12-01") - 90, Day("1998-09-02"));
  EXPECT_EQ(AddInterval(Day("1998-12-01"), -90, IntervalUnit::kDay).ValueOrDie(),
            Day("1998-09-02"));
  EXPECT_EQ(AddInterval(Day("1996-02-28"), 2, IntervalUnit::kDay).ValueOrDie(), Day("1996-03-01"));
}

TEST(DateTest, MonthAndYearStepsClampToTheLastDay) {
  EXPECT_EQ(AddInterval(Day("1994-01-31"), 1, IntervalUnit::kMonth).ValueOrDie(),
            Day("1994-02-28"));
  EXPECT_EQ(AddInterval(Day("1996-01-31"), 1, IntervalUnit::kMonth).ValueOrDie(),
            Day("1996-02-29"));
  EXPECT_EQ(AddInterval(Day("1996-03-31"), -1, IntervalUnit::kMonth).ValueOrDie(),
            Day("1996-02-29"));
  EXPECT_EQ(AddInterval(Day("1996-02-29"), 1, IntervalUnit::kYear).ValueOrDie(), Day("1997-02-28"));
  EXPECT_EQ(AddInterval(Day("1994-01-01"), 3, IntervalUnit::kMonth).ValueOrDie(),
            Day("1994-04-01"));
  EXPECT_EQ(AddInterval(Day("1994-11-15"), 3, IntervalUnit::kMonth).ValueOrDie(),
            Day("1995-02-15"));
  EXPECT_EQ(AddInterval(Day("1995-01-15"), -1, IntervalUnit::kMonth).ValueOrDie(),
            Day("1994-12-15"));
  EXPECT_EQ(AddInterval(Day("1994-01-01"), 1, IntervalUnit::kYear).ValueOrDie(), Day("1995-01-01"));
  EXPECT_EQ(AddInterval(Day("1994-01-01"), -25, IntervalUnit::kMonth).ValueOrDie(),
            Day("1991-12-01"));
}

TEST(DateTest, RejectsMalformedAndImpossibleDates) {
  for (const char* bad : {"1994-02-30", "1994-02-29", "1994-13-01", "1994-00-10", "1994-04-31",
                          "1994-1-01", "94-01-01", "1994/01/01", "1994-01-011", "", "abcd-ef-gh",
                          "0000-01-01", "1994-01-00", " 1994-01-01"}) {
    const auto parsed = ParseDate(bad);
    EXPECT_TRUE(parsed.status().IsInvalid()) << bad;
  }
  EXPECT_TRUE(ParseDate("1996-02-29").ok());
  EXPECT_TRUE(ParseDate("2000-02-29").ok());
  EXPECT_TRUE(ParseDate("1900-02-29").status().IsInvalid());
}

TEST(DateTest, RoundTripsEveryDayOfARange) {
  for (int32_t day = Day("1899-12-25"); day < Day("2101-01-05"); ++day) {
    ASSERT_EQ(Day(FormatDate(day).c_str()), day) << FormatDate(day);
  }
  EXPECT_EQ(FormatDate(0), "1970-01-01");
  EXPECT_EQ(FormatDate(Day("1998-09-02")), "1998-09-02");
}

TEST(DateTest, IntervalsOutOfRangeAreInvalid) {
  EXPECT_TRUE(AddInterval(Day("9999-12-31"), 1, IntervalUnit::kDay).status().IsInvalid());
  EXPECT_TRUE(AddInterval(Day("9999-12-31"), 1, IntervalUnit::kYear).status().IsInvalid());
  EXPECT_TRUE(AddInterval(0, INT64_MAX, IntervalUnit::kMonth).status().IsInvalid());
  EXPECT_TRUE(AddInterval(0, INT64_MAX, IntervalUnit::kYear).status().IsInvalid());
  EXPECT_TRUE(AddInterval(0, INT64_MIN, IntervalUnit::kDay).status().IsInvalid());
}

}  // namespace
}  // namespace quarry
