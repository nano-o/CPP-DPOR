#include "simulation.hpp"
#include "two_phase_commit_benchmark.hpp"

int main(int argc, char** argv) {
  return dpor::benchmarks::run_two_phase_commit_benchmark(
      argc, argv, "2PC timeout benchmark", [](std::size_t participants, bool inject_crash) {
        return tpc_sim::make_two_phase_commit_program(participants, inject_crash);
      });
}
