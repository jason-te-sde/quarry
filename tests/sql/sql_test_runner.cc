// Runs the SQL error tests in tests/sql/*.test against the TPC-H test data.
//
// A file is a list of blocks separated by blank lines; lines starting with '#' outside a block are
// comments. A block is a header line followed by the SQL text (any number of lines):
//
//   error <kind> <line>:<column> [text]   the statement must fail with the message prefix
//                                         "<kind> error at <line>:<column>: " and, when `text` is
//                                         given, a message that contains it
//   ok                                    the statement must parse and bind
//
// <kind> is syntax or binder.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "binder/catalog.h"

namespace quarry {
namespace {

struct Block {
  int header_line = 0;
  std::string header;
  std::string sql;
};

std::vector<Block> ReadBlocks(const std::filesystem::path& file) {
  std::ifstream in(file);
  EXPECT_TRUE(in.good()) << file;
  std::vector<Block> blocks;
  std::string line;
  int number = 0;
  Block current;
  bool open = false;
  const auto close = [&] {
    if (open) blocks.push_back(std::move(current));
    current = Block();
    open = false;
  };
  while (std::getline(in, line)) {
    ++number;
    if (line.empty()) {
      close();
    } else if (!open && line[0] == '#') {
      continue;
    } else if (!open) {
      open = true;
      current.header_line = number;
      current.header = line;
    } else {
      if (!current.sql.empty()) current.sql += '\n';
      current.sql += line;
    }
  }
  close();
  return blocks;
}

const Catalog& TestCatalog() {
  static const Catalog* catalog = [] {
    auto* c = new Catalog();
    const arrow::Status st = c->AddDirectory("tests/data/tpch-sf0.01");
    EXPECT_TRUE(st.ok()) << st.ToString();
    return c;
  }();
  return *catalog;
}

void RunBlock(const std::filesystem::path& file, const Block& block) {
  SCOPED_TRACE(file.string() + ":" + std::to_string(block.header_line) + "\n" + block.sql);
  std::istringstream header(block.header);
  std::string word;
  header >> word;
  const auto result = ParseAndBind(block.sql, TestCatalog());
  if (word == "ok") {
    EXPECT_TRUE(result.ok()) << result.status().ToString();
    return;
  }
  ASSERT_EQ(word, "error") << "unknown block header: " << block.header;
  std::string kind;
  std::string position;
  header >> kind >> position;
  std::string needle;
  std::getline(header, needle);
  while (!needle.empty() && needle.front() == ' ') needle.erase(needle.begin());
  ASSERT_FALSE(kind.empty());
  ASSERT_FALSE(position.empty());
  ASSERT_FALSE(result.ok()) << "expected an error";
  const std::string message = result.status().message();
  const std::string prefix = kind + " error at " + position + ": ";
  EXPECT_EQ(message.rfind(prefix, 0), 0u)
      << "expected prefix: " << prefix << "\n  got: " << message;
  if (!needle.empty()) {
    EXPECT_NE(message.find(needle), std::string::npos)
        << "expected to contain: " << needle << "\n  got: " << message;
  }
}

TEST(SqlTest, EveryBlockOfEveryFile) {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator("tests/sql")) {
    if (entry.path().extension() == ".test") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  ASSERT_FALSE(files.empty());
  for (const auto& file : files) {
    const std::vector<Block> blocks = ReadBlocks(file);
    EXPECT_FALSE(blocks.empty()) << file;
    for (const Block& block : blocks) RunBlock(file, block);
  }
}

}  // namespace
}  // namespace quarry
