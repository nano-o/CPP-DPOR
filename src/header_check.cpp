// Internal translation unit for the header-only dpor library.
//
// This file is compiled only so warning and static-analysis settings can be
// enforced on the public headers without turning the installed target into a
// non-header-only library.

#include "dpor/algo/dpor.hpp"
#include "dpor/algo/program.hpp"
#include "dpor/model/consistency.hpp"
#include "dpor/model/event.hpp"
#include "dpor/model/execution_graph.hpp"
#include "dpor/model/exploration_graph.hpp"
#include "dpor/model/relation.hpp"

namespace dpor::build {
void header_check_anchor() {}
}  // namespace dpor::build
