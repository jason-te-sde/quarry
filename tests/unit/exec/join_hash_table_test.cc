#include "exec/join_hash_table.h"

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace quarry {
namespace {

using ArrayPtr = std::shared_ptr<arrow::Array>;

ArrayPtr Ints(const std::vector<int64_t>& values, const std::set<size_t>& nulls = {}) {
  arrow::Int64Builder builder;
  for (size_t i = 0; i < values.size(); ++i) {
    if (nulls.count(i) > 0) {
      EXPECT_TRUE(builder.AppendNull().ok());
    } else {
      EXPECT_TRUE(builder.Append(values[i]).ok());
    }
  }
  ArrayPtr out;
  EXPECT_TRUE(builder.Finish(&out).ok());
  return out;
}

ArrayPtr Strings(const std::vector<std::string>& values) {
  arrow::StringBuilder builder;
  for (const std::string& v : values) EXPECT_TRUE(builder.Append(v).ok());
  ArrayPtr out;
  EXPECT_TRUE(builder.Finish(&out).ok());
  return out;
}

// The (probe row -> build rows) matches as a sorted multimap.
std::map<uint32_t, std::vector<uint32_t>> Matches(const JoinHashTable& table,
                                                  const std::vector<ArrayPtr>& keys, int64_t n) {
  std::vector<uint32_t> probe;
  std::vector<uint32_t> build;
  EXPECT_TRUE(table.Probe(keys, n, &probe, &build).ok());
  EXPECT_EQ(probe.size(), build.size());
  std::map<uint32_t, std::vector<uint32_t>> out;
  for (size_t i = 0; i < probe.size(); ++i) out[probe[i]].push_back(build[i]);
  for (auto& entry : out) std::sort(entry.second.begin(), entry.second.end());
  return out;
}

TEST(JoinHashTableTest, ConstantHashStillMatchesByKey) {
  JoinHashTable table([](std::span<const std::byte>) -> uint64_t { return 42; });
  std::vector<int64_t> build;
  for (int64_t k = 0; k < 1000; ++k) build.push_back(k * 7);
  ASSERT_TRUE(table.Insert({Ints(build)}, 1000).ok());
  ASSERT_EQ(table.rows(), 1000);

  // Every key is probed, in reverse, plus keys that are absent.
  std::vector<int64_t> probe;
  for (int64_t k = 999; k >= 0; --k) probe.push_back(k * 7);
  probe.push_back(1);
  probe.push_back(-5);
  const auto matches = Matches(table, {Ints(probe)}, static_cast<int64_t>(probe.size()));
  ASSERT_EQ(matches.size(), 1000u);
  for (int64_t i = 0; i < 1000; ++i) {
    const auto it = matches.find(static_cast<uint32_t>(i));
    ASSERT_NE(it, matches.end()) << "probe row " << i;
    ASSERT_EQ(it->second.size(), 1u);
    EXPECT_EQ(it->second[0], static_cast<uint32_t>(999 - i));  // build row of key (999-i)*7
  }
}

TEST(JoinHashTableTest, DuplicateBuildKeysReturnAllMatches) {
  JoinHashTable table;
  ASSERT_TRUE(table.Insert({Ints({5, 6, 5, 5, 7})}, 5).ok());
  ASSERT_TRUE(table.Insert({Ints({6, 5})}, 2).ok());  // a second batch continues the numbering
  const auto matches = Matches(table, {Ints({5, 6, 8, 5})}, 4);
  EXPECT_EQ(matches.at(0), (std::vector<uint32_t>{0, 2, 3, 6}));
  EXPECT_EQ(matches.at(1), (std::vector<uint32_t>{1, 5}));
  EXPECT_EQ(matches.count(2), 0u);
  EXPECT_EQ(matches.at(3), (std::vector<uint32_t>{0, 2, 3, 6}));
}

TEST(JoinHashTableTest, TheChainIsWalkedInInsertionOrder) {
  JoinHashTable table;
  ASSERT_TRUE(table.Insert({Ints({1, 2, 1, 1, 2, 1})}, 6).ok());
  std::vector<uint32_t> first;
  ASSERT_TRUE(table.Lookup({Ints({1, 2, 3})}, 3, &first).ok());
  std::vector<uint32_t> ones;
  for (uint32_t r = first[0]; r != JoinHashTable::kNoRow; r = table.Next(r)) ones.push_back(r);
  EXPECT_EQ(ones, (std::vector<uint32_t>{0, 2, 3, 5}));
  EXPECT_EQ(first[2], JoinHashTable::kNoRow);
}

TEST(JoinHashTableTest, NullKeysNeverMatch) {
  JoinHashTable table;
  ASSERT_TRUE(table.Insert({Ints({1, 0, 2, 0}, {1, 3})}, 4).ok());
  EXPECT_EQ(table.rows(), 4);  // NULL-key rows are numbered, never matched
  const auto matches = Matches(table, {Ints({0, 1, 0, 2}, {0, 2})}, 4);
  EXPECT_EQ(matches.count(0), 0u);  // a NULL probe key
  EXPECT_EQ(matches.count(2), 0u);
  EXPECT_EQ(matches.at(1), (std::vector<uint32_t>{0}));
  EXPECT_EQ(matches.at(3), (std::vector<uint32_t>{2}));
  // A stored 0 is not a NULL: probing the value 0 finds nothing, because the 0s were NULLs.
  EXPECT_TRUE(Matches(table, {Ints({0})}, 1).empty());
}

TEST(JoinHashTableTest, MultiColumnKeys) {
  JoinHashTable table;
  ASSERT_TRUE(table.Insert({Ints({1, 1, 2, 2}), Strings({"a", "b", "a", "a"})}, 4).ok());
  const auto matches = Matches(table, {Ints({2, 1, 1, 3}), Strings({"a", "b", "c", "a"})}, 4);
  EXPECT_EQ(matches.at(0), (std::vector<uint32_t>{2, 3}));
  EXPECT_EQ(matches.at(1), (std::vector<uint32_t>{1}));
  EXPECT_EQ(matches.count(2), 0u);
  EXPECT_EQ(matches.count(3), 0u);
}

TEST(JoinHashTableTest, EmptyStringsAndPrefixesAreDifferentKeys) {
  JoinHashTable table;
  ASSERT_TRUE(table.Insert({Strings({"", "a", "ab"}), Strings({"x", "bx", "x"})}, 3).ok());
  const auto matches = Matches(table, {Strings({"a", "", "ab"}), Strings({"bx", "x", "x"})}, 3);
  EXPECT_EQ(matches.at(0), (std::vector<uint32_t>{1}));
  EXPECT_EQ(matches.at(1), (std::vector<uint32_t>{0}));
  EXPECT_EQ(matches.at(2), (std::vector<uint32_t>{2}));
}

TEST(JoinHashTableTest, GrowsPastInitialCapacity) {
  JoinHashTable table;
  constexpr int64_t kKeys = 50000;
  std::vector<int64_t> build;
  for (int64_t k = 0; k < kKeys; ++k) build.push_back(k % 20000);
  for (int64_t start = 0; start < kKeys; start += 4096) {
    const int64_t n = std::min<int64_t>(4096, kKeys - start);
    ASSERT_TRUE(
        table
            .Insert({Ints(std::vector<int64_t>(build.begin() + start, build.begin() + start + n))},
                    n)
            .ok());
  }
  EXPECT_EQ(table.rows(), kKeys);
  EXPECT_GT(table.memory_bytes(), 0);
  std::vector<int64_t> probe;
  for (int64_t k = 0; k < 20000; ++k) probe.push_back(k);
  probe.push_back(20000);
  const auto matches = Matches(table, {Ints(probe)}, static_cast<int64_t>(probe.size()));
  ASSERT_EQ(matches.size(), 20000u);
  EXPECT_EQ(matches.at(0).size(), 3u);      // keys 0..9999 appear three times
  EXPECT_EQ(matches.at(15000).size(), 2u);  // keys 10000..19999 twice
  for (const auto& entry : matches) {
    for (const uint32_t row : entry.second) {
      EXPECT_EQ(build[row], static_cast<int64_t>(entry.first));
    }
  }
}

TEST(JoinHashTableTest, DoubleZerosAreEqualKeys) {
  arrow::DoubleBuilder builder;
  ASSERT_TRUE(builder.Append(0.0).ok());
  ArrayPtr zero;
  ASSERT_TRUE(builder.Finish(&zero).ok());
  ASSERT_TRUE(builder.Append(-0.0).ok());
  ArrayPtr negative_zero;
  ASSERT_TRUE(builder.Finish(&negative_zero).ok());
  JoinHashTable table;
  ASSERT_TRUE(table.Insert({zero}, 1).ok());
  EXPECT_EQ(Matches(table, {negative_zero}, 1).at(0), (std::vector<uint32_t>{0}));
}

// A growth is announced before it is allocated: the table asks the charge function for the bytes of
// new storage (a rehash holds the old slot array and the new one together), and a refusal leaves
// the table exactly as it was.
TEST(JoinHashTableTest, StorageIsChargedBeforeItIsAllocatedAndARefusalInsertsNothing) {
  arrow::ProxyMemoryPool pool(arrow::default_memory_pool());
  JoinHashTable table({}, &pool);
  std::vector<int64_t> keys(5000);
  for (size_t i = 0; i < keys.size(); ++i) keys[i] = static_cast<int64_t>(i);

  int64_t requested = 0;
  int64_t pool_at_request = -1;
  const auto agree = [&](int64_t bytes) {
    requested += bytes;
    pool_at_request = pool.bytes_allocated();
    return true;
  };
  const int64_t before_bytes = pool.bytes_allocated();
  auto inserted = table.Insert({Ints(keys)}, 5000, agree);
  ASSERT_TRUE(inserted.ok());
  EXPECT_TRUE(*inserted);
  EXPECT_GT(requested, 0);
  EXPECT_EQ(pool_at_request, before_bytes) << "the charge came after the allocation";
  EXPECT_GE(requested, pool.bytes_allocated() - before_bytes)
      << "the table allocated more than it announced";
  EXPECT_EQ(table.rows(), 5000);

  const int64_t held = table.memory_bytes();
  const int64_t allocated = pool.bytes_allocated();
  std::vector<int64_t> more(20000);
  for (size_t i = 0; i < more.size(); ++i) more[i] = 100000 + static_cast<int64_t>(i);
  auto refused = table.Insert({Ints(more)}, 20000, [](int64_t) { return false; });
  ASSERT_TRUE(refused.ok());
  EXPECT_FALSE(*refused);
  EXPECT_EQ(table.rows(), 5000);
  EXPECT_EQ(table.memory_bytes(), held);
  EXPECT_EQ(pool.bytes_allocated(), allocated);
}

}  // namespace
}  // namespace quarry
