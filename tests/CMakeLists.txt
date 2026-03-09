add_executable(dpor_session_test session_test.cpp)
target_link_libraries(dpor_session_test PRIVATE dpor::dpor Catch2::Catch2WithMain)

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

include(Catch)
catch_discover_tests(dpor_session_test)
catch_discover_tests(dpor_model_types_test)
catch_discover_tests(dpor_relation_test)
catch_discover_tests(dpor_consistency_test)
catch_discover_tests(dpor_exploration_graph_test)
catch_discover_tests(dpor_dpor_test)
catch_discover_tests(dpor_dpor_stress_test)
