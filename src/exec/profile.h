#ifndef QUARRY_EXEC_PROFILE_H_
#define QUARRY_EXEC_PROFILE_H_

#include <cstdint>
#include <string>

namespace quarry {

// What one operator did, for EXPLAIN ANALYZE. exclusive_ns is the operator's own time: the time
// spent inside its Open, Next and Close minus the time spent inside its inputs'.
struct OperatorStats {
  int id = 0;
  std::string name;
  int64_t rows_out = 0;
  int64_t batches_out = 0;
  int64_t exclusive_ns = 0;
  int64_t peak_state_bytes = 0;
  int64_t spilled_bytes = 0;
  int64_t scan_buffer_bytes = 0;
};

}  // namespace quarry

#endif  // QUARRY_EXEC_PROFILE_H_
