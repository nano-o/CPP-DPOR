add_executable(dpor_two_phase_commit_timeout_test two_phase_commit_test.cpp)
target_link_libraries(dpor_two_phase_commit_timeout_test PRIVATE dpor::dpor Catch2::Catch2WithMain)
target_include_directories(dpor_two_phase_commit_timeout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED Catch2_SOURCE_DIR)
  list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
endif()

include(Catch)
catch_discover_tests(dpor_two_phase_commit_timeout_test)
