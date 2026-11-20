#ifndef QUARRY_COMMON_FLAWS_H_
#define QUARRY_COMMON_FLAWS_H_

// The one place that turns the QUARRY_FLAW_* compile definitions into constants. A build with
// -DQUARRY_FLAW=<name> compiles exactly one deliberate defect in, so that the test suite can prove
// it would notice that kind of bug (scripts/run-flaws.sh). Each flag guards a single
// `if constexpr` at the point of the defect; no other file reads the definitions.

namespace quarry {

#ifdef QUARRY_FLAW_SELECTION_OFF_BY_ONE
// The expression evaluator gathers one row too far for the last row of a selection vector.
inline constexpr bool kFlawSelectionOffByOne = true;
#else
inline constexpr bool kFlawSelectionOffByOne = false;
#endif

#ifdef QUARRY_FLAW_NULL_AS_FALSE
// NOT of NULL returns TRUE, as if NULL had been FALSE.
inline constexpr bool kFlawNullAsFalse = true;
#else
inline constexpr bool kFlawNullAsFalse = false;
#endif

#ifdef QUARRY_FLAW_DECIMAL_SCALE_SLIP
// A decimal rescale by two or more digits moves one digit too few.
inline constexpr bool kFlawDecimalScaleSlip = true;
#else
inline constexpr bool kFlawDecimalScaleSlip = false;
#endif

}  // namespace quarry

#endif  // QUARRY_COMMON_FLAWS_H_
