#include "exec/memory_budget.h"

#include <gtest/gtest.h>

#include <memory>

namespace quarry {
namespace {

TEST(MemoryBudgetTest, ZeroMeansUnlimited) {
  MemoryBudget budget(0);
  Reservation reservation(budget, "op");
  EXPECT_TRUE(reservation.Grow(int64_t{1} << 40).ok());
  EXPECT_EQ(budget.used(), int64_t{1} << 40);
  EXPECT_EQ(budget.limit(), 0);
}

TEST(MemoryBudgetTest, GrowBeyondTheLimitIsResourceExhausted) {
  MemoryBudget budget(1000);
  Reservation a(budget, "HashJoin #2");
  ASSERT_TRUE(a.Grow(600).ok());
  const arrow::Status status = a.Grow(500);
  ASSERT_TRUE(status.IsOutOfMemory());
  EXPECT_EQ(status.message(),
            "resource exhausted: HashJoin #2 needs 500 bytes, budget 1000, in use 600");
  EXPECT_EQ(a.size(), 600);  // a failed Grow charges nothing
}

TEST(MemoryBudgetTest, TryGrowReturnsFalseInsteadOfFailing) {
  MemoryBudget budget(100);
  Reservation a(budget, "a");
  EXPECT_TRUE(a.TryGrow(100));
  EXPECT_FALSE(a.TryGrow(1));
  a.Shrink(40);
  EXPECT_TRUE(a.TryGrow(40));
  EXPECT_EQ(budget.used(), 100);
}

TEST(MemoryBudgetTest, ReservationsShareTheBudgetAndReturnItOnDestruction) {
  MemoryBudget budget(100);
  {
    Reservation a(budget, "a");
    Reservation b(budget, "b");
    ASSERT_TRUE(a.Grow(60).ok());
    EXPECT_FALSE(b.TryGrow(60));
    ASSERT_TRUE(b.Grow(40).ok());
    EXPECT_EQ(budget.used(), 100);
    EXPECT_EQ(budget.peak(), 100);
  }
  EXPECT_EQ(budget.used(), 0);
  EXPECT_EQ(budget.peak(), 100);
}

TEST(MemoryBudgetTest, ShrinkNeverGoesBelowZeroAndPeakIsPerReservation) {
  MemoryBudget budget(0);
  Reservation a(budget, "a");
  ASSERT_TRUE(a.Grow(50).ok());
  a.Shrink(500);
  EXPECT_EQ(a.size(), 0);
  EXPECT_EQ(budget.used(), 0);
  EXPECT_EQ(a.peak(), 50);
}

TEST(MemoryBudgetTest, AReservationMayOutliveItsBudget) {
  // An operator destroyed after the budget it reserved from must not touch freed memory: the
  // reservation shares ownership of the budget's state. (AddressSanitizer flags the dangling
  // access when it does not.)
  auto budget = std::make_unique<MemoryBudget>(100);
  Reservation a(*budget, "a");
  ASSERT_TRUE(a.Grow(60).ok());
  budget.reset();
  EXPECT_FALSE(a.TryGrow(60));  // the limit still applies
  EXPECT_TRUE(a.TryGrow(40));
  a.Shrink(100);
  EXPECT_EQ(a.size(), 0);
}

TEST(MemoryBudgetTest, ReservationsOfDifferentBudgetsAreIndependent) {
  MemoryBudget small(10);
  MemoryBudget large(1000);
  Reservation a(small, "a");
  Reservation b(large, "b");
  EXPECT_FALSE(a.TryGrow(11));
  EXPECT_TRUE(b.TryGrow(500));
  EXPECT_EQ(small.used(), 0);
  EXPECT_EQ(large.used(), 500);
}

}  // namespace
}  // namespace quarry
