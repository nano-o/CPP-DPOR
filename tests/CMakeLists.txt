option(DPOR_FETCH_CATCH2 "Fetch Catch2 from GitHub if not found locally" OFF)

find_package(Catch2 3 CONFIG QUIET)

if(NOT Catch2_FOUND)
  if(DPOR_FETCH_CATCH2)
    include(FetchContent)
    FetchContent_Declare(
      Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.8.0
    )
    if(NOT EXISTS "${FETCHCONTENT_BASE_DIR}/catch2-src")
      message(STATUS "Catch2 v3 not found locally; fetching with FetchContent")
    endif()
    FetchContent_MakeAvailable(Catch2)
  else()
    message(
      FATAL_ERROR
        "Catch2 v3 not found. Install Catch2 v3 locally, configure with -DDPOR_FETCH_CATCH2=ON, or disable tests with -DDPOR_BUILD_TESTING=OFF."
    )
  endif()
endif()

add_executable(dpor_session_test session_test.cpp)
target_link_libraries(dpor_session_test PRIVATE dpor::dpor Catch2::Catch2WithMain)

add_executable(dpor_model_types_test model_types_test.cpp)
target_link_libraries(dpor_model_types_test PRIVATE dpor::dpor Catch2::Catch2WithMain)

add_executable(dpor_relation_test relation_test.cpp)
target_link_libraries(dpor_relation_test PRIVATE dpor::dpor Catch2::Catch2WithMain)

add_executable(dpor_consistency_test consistency_test.cpp)
target_link_libraries(dpor_consistency_test PRIVATE dpor::dpor Catch2::Catch2WithMain)

if(DEFINED Catch2_SOURCE_DIR)
  list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
endif()

include(Catch)
catch_discover_tests(dpor_session_test)
catch_discover_tests(dpor_model_types_test)
catch_discover_tests(dpor_relation_test)
catch_discover_tests(dpor_consistency_test)
