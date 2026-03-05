cmake_minimum_required(VERSION 3.22)

project(
  dpor
  VERSION 0.1.0
  DESCRIPTION "DPOR model checking library for distributed systems"
  LANGUAGES CXX
)

include(GNUInstallDirs)
include(CTest)

option(DPOR_BUILD_TESTING "Build test targets" ON)
option(DPOR_BUILD_EXAMPLES "Build example targets" ON)
option(DPOR_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
option(DPOR_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

if(DPOR_ENABLE_WARNINGS)
  include(cmake/CompilerWarnings.cmake)
endif()

add_library(dpor)
add_library(dpor::dpor ALIAS dpor)

target_sources(
  dpor
  PRIVATE
    src/api/session.cpp
)

target_compile_features(dpor PUBLIC cxx_std_20)

set_target_properties(
  dpor
  PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME dpor
)

target_include_directories(
  dpor
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(DPOR_ENABLE_WARNINGS)
  dpor_set_project_warnings(dpor)
endif()

install(
  TARGETS dpor
  EXPORT dporTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
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
  enable_testing()
  add_subdirectory(tests)
endif()

if(DPOR_BUILD_EXAMPLES)
  add_subdirectory(examples/minimal)
  if(DPOR_BUILD_TESTING)
    add_subdirectory(examples/two_phase_commit)
    add_subdirectory(examples/two_phase_commit_timeout)
  endif()
endif()
