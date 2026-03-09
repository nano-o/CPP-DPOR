add_executable(dpor_two_phase_commit_timeout_benchmark main.cpp)
target_link_libraries(dpor_two_phase_commit_timeout_benchmark PRIVATE dpor::dpor)
target_include_directories(
  dpor_two_phase_commit_timeout_benchmark
  PRIVATE
    ${PROJECT_SOURCE_DIR}/benchmarks/common
    ${PROJECT_SOURCE_DIR}/examples/two_phase_commit_timeout
    ${PROJECT_SOURCE_DIR}/tests
)
dpor_apply_static_analysis(dpor_two_phase_commit_timeout_benchmark)
