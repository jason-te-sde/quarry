#include "plan/physical.h"

namespace quarry {

const char* PhysicalKindName(PhysicalKind kind) {
  switch (kind) {
    case PhysicalKind::kScan:
      return "Scan";
    case PhysicalKind::kFilter:
      return "Filter";
    case PhysicalKind::kProject:
      return "Project";
    case PhysicalKind::kHashJoin:
      return "HashJoin";
    case PhysicalKind::kHashAggregate:
      return "HashAggregate";
    case PhysicalKind::kSort:
      return "Sort";
    case PhysicalKind::kTopN:
      return "TopN";
    case PhysicalKind::kLimit:
      return "Limit";
  }
  return "?";
}

}  // namespace quarry
