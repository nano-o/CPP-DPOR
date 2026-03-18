cmake_minimum_required(VERSION 3.22)

project(
  dpor
  VERSION 0.1.0
  DESCRIPTION "DPOR model checking library for distributed systems"
  LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CTest)

option(DPOR_BUILD_TESTING "Build test targets" ON)
option(DPOR_BUILD_EXAMPLES "Build example targets" ON)
option(DPOR_BUILD_BENCHMARKS "Build standalone benchmark targets" ON)
option(DPOR_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
option(DPOR_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

if(DPOR_ENABLE_WARNINGS)
  include(cmake/CompilerWarnings.cmake)
endif()

include(cmake/StaticAnalysis.cmake)

add_library(dpor INTERFACE)
add_library(dpor::dpor ALIAS dpor)

target_compile_features(dpor INTERFACE cxx_std_20)

set_target_properties(
  dpor
  PROPERTIES
    EXPORT_NAME dpor
)

target_include_directories(
  dpor
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Internal build target for the header-only library. This gives warnings and
# static-analysis tools a concrete translation unit to compile without changing
# the public package shape.
add_library(dpor_header_check OBJECT src/header_check.cpp)
target_link_libraries(dpor_header_check PRIVATE dpor::dpor)
if(DPOR_ENABLE_WARNINGS)
  dpor_set_project_warnings(dpor_header_check)
endif()
dpor_apply_static_analysis(dpor_header_check)

install(
  TARGETS dpor
  EXPORT dporTargets
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  EXPORT dporTargets
  NAMESPACE dpor::
  FILE dporTargets.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dpor
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/dporConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dporConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/dporConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dpor
)

install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/dporConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/dporConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dpor
)

if(DPOR_BUILD_TESTING)
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

  # Make the Catch test-discovery module available to all subdirectories.
  if(DEFINED Catch2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
  else()
    get_target_property(_catch2_config_dir Catch2::Catch2 INTERFACE_INCLUDE_DIRECTORIES)
    find_file(_catch_module Catch.cmake
      PATHS "${Catch2_DIR}" "${CMAKE_PREFIX_PATH}"
      PATH_SUFFIXES cmake/Catch2 lib/cmake/Catch2
      NO_DEFAULT_PATH
    )
    if(_catch_module)
      get_filename_component(_catch_module_dir "${_catch_module}" DIRECTORY)
      list(APPEND CMAKE_MODULE_PATH "${_catch_module_dir}")
    endif()
  endif()

  include(Catch)

  function(dpor_discover_catch_tests target)
    catch_discover_tests(${target}
      TEST_PREFIX "${target}::"
    )
  endfunction()

  enable_testing()
  add_subdirectory(tests)
endif()

if(DPOR_BUILD_EXAMPLES)
  if(DPOR_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks/two_phase_commit_timeout)
  endif()
  if(DPOR_BUILD_TESTING)
    add_subdirectory(examples/two_phase_commit_timeout)
  endif()
endif()
