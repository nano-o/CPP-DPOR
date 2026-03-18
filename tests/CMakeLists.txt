add_executable(dpor_model_types_test model_types_test.cpp)
target_link_libraries(dpor_model_types_test PRIVATE dpor::dpor Catch2::Catch2WithMain)

add_executable(dpor_relation_test relation_test.cpp)
target_link_libraries(dpor_relation_test PRIVATE dpor::dpor Catch2::Catch2WithMain)

add_executable(dpor_consistency_test consistency_test.cpp)
target_link_libraries(dpor_consistency_test PRIVATE dpor::dpor Catch2::Catch2WithMain)

add_executable(dpor_exploration_graph_test exploration_graph_test.cpp)
target_link_libraries(dpor_exploration_graph_test PRIVATE dpor::dpor Catch2::Catch2WithMain)

add_executable(dpor_dpor_test dpor_test.cpp)
target_link_libraries(dpor_dpor_test PRIVATE dpor::dpor Catch2::Catch2WithMain)

add_executable(dpor_dpor_stress_test dpor_stress_test.cpp)
target_link_libraries(dpor_dpor_stress_test PRIVATE dpor::dpor Catch2::Catch2WithMain)

set(DPOR_TEST_TARGETS
  dpor_model_types_test
  dpor_relation_test
  dpor_consistency_test
  dpor_exploration_graph_test
  dpor_dpor_test
  dpor_dpor_stress_test
)

foreach(target IN LISTS DPOR_TEST_TARGETS)
  dpor_apply_static_analysis(${target})
endforeach()

foreach(target IN LISTS DPOR_TEST_TARGETS)
  dpor_discover_catch_tests(${target})
endforeach()
