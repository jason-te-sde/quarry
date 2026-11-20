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

#ifdef QUARRY_FLAW_JOIN_ORDER_DROPS_PREDICATE
// When order_joins adds a relation that is reachable by two key pairs, it keeps only the first one:
// the other equality is lost.
inline constexpr bool kFlawJoinOrderDropsPredicate = true;
#else
inline constexpr bool kFlawJoinOrderDropsPredicate = false;
#endif

#ifdef QUARRY_FLAW_DROP_HASH_COLLISIONS
// A hash table probe gives up at the first occupied slot that does not hold the key, so keys that
// collide with an earlier one are lost.
inline constexpr bool kFlawDropHashCollisions = true;
#else
inline constexpr bool kFlawDropHashCollisions = false;
#endif

#ifdef QUARRY_FLAW_PRUNE_WRONG_BOUND
// Row-group pruning treats <= as < when it compares a literal with a row group's maximum, so a
// group whose maximum equals the literal is skipped although it holds matching rows.
inline constexpr bool kFlawPruneWrongBound = true;
#else
inline constexpr bool kFlawPruneWrongBound = false;
#endif

#ifdef QUARRY_FLAW_SPILL_DROPS_LAST_BATCH
// A spill writer does not flush its final buffered rows before it finishes the file, so the tail of
// every spilled partition is lost.
inline constexpr bool kFlawSpillDropsLastBatch = true;
#else
inline constexpr bool kFlawSpillDropsLastBatch = false;
#endif

}  // namespace quarry

#endif  // QUARRY_COMMON_FLAWS_H_
