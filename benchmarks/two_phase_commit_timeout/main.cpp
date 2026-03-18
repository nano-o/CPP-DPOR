#include "sim/crash_before_decision.hpp"
#include "two_phase_commit_benchmark.hpp"

int main(int argc, char** argv) {
  return dpor::benchmarks::run_two_phase_commit_benchmark(
      argc, argv, "2PC timeout benchmark", [](std::size_t participants, bool inject_crash) {
        return tpc_sim::crash_before_decision::make_program({
            .num_participants = participants,
            .inject_crash = inject_crash,
        });
      });
}
