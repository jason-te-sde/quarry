// Runs the four TPC-H queries on the SF0.01 data and compares the JSON of the result with the
// answers DuckDB produced (tests/tpch/answers/q*.json, written by scripts/gen_tpch.py). Each query
// runs with the default options, with every optimizer rule switched off on its own, and with
// batches of 1 and 7 rows, so that the result has to be the same whichever plan and whichever
// batch boundaries the engine happens to use.
//
// The answers have a total order (no two rows tie on the ORDER BY keys, also not at Q3's LIMIT 10
// boundary: row 11 has a revenue of 195185.6655 against 201502.2188 of row 10), so the rows are
// compared in order.

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "output_format.h"
#include "quarry/engine.h"
#include "types/logical_type.h"

namespace quarry {
namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << path;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The answer files end with ,"duckdb_version":"..."}, which is not part of the result: cut it.
std::string ColumnsAndRows(std::string json) {
  while (!json.empty() && (json.back() == '\n' || json.back() == '\r')) json.pop_back();
  const size_t version = json.find(",\"duckdb_version\"");
  if (version != std::string::npos) json = json.substr(0, version) + "}";
  return json;
}

struct Config {
  std::string name;
  EngineOptions options;
};

std::vector<Config> Configs() {
  std::vector<Config> configs;
  configs.push_back({"default", EngineOptions{}});
  for (const char* rule : {"fold_constants", "push_filters", "prune_row_groups", "prune_columns",
                           "order_joins", "pick_build_side"}) {
    EngineOptions options;
    options.disabled_rules = {rule};
    configs.push_back({std::string("without_") + rule, options});
  }
  for (const int32_t batch : {1, 7}) {
    EngineOptions options;
    options.batch_size = batch;
    configs.push_back({"batch_size_" + std::to_string(batch), options});
  }
  return configs;
}

struct Case {
  int query;
  Config config;
};

void PrintTo(const Case& c, std::ostream* os) { *os << "q" << c.query << " " << c.config.name; }

class TpchTest : public ::testing::TestWithParam<Case> {};

TEST_P(TpchTest, ResultMatchesDuckDB) {
  const Case& c = GetParam();
  Engine engine(c.config.options);
  const arrow::Status registered = engine.RegisterDirectory("tests/data/tpch-sf0.01");
  ASSERT_TRUE(registered.ok()) << registered.ToString();
  const std::string sql = ReadFile("tests/tpch/queries/q" + std::to_string(c.query) + ".sql");
  auto result = engine.Execute(sql);
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  const std::string expected =
      ColumnsAndRows(ReadFile("tests/tpch/answers/q" + std::to_string(c.query) + ".json"));
  EXPECT_EQ(ToJson(*result->table), expected);
}

std::vector<Case> AllCases() {
  std::vector<Case> cases;
  for (const int query : {1, 3, 5, 6}) {
    for (const Config& config : Configs()) cases.push_back({query, config});
  }
  return cases;
}

INSTANTIATE_TEST_SUITE_P(Answers, TpchTest, ::testing::ValuesIn(AllCases()),
                         [](const ::testing::TestParamInfo<Case>& info) {
                           return "q" + std::to_string(info.param.query) + "_" +
                                  info.param.config.name;
                         });

}  // namespace
}  // namespace quarry
