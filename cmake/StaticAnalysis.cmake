# cmake/StaticAnalysis.cmake
# Optional static-analysis tool integration.
# Each tool is guarded by a DPOR_ENABLE_<TOOL> option so users can opt in
# to exactly the tools they have installed.
#
# Tools are applied per-target (not globally) to avoid analyzing third-party
# code from FetchContent dependencies.

option(DPOR_ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)
option(DPOR_ENABLE_CPPCHECK "Run cppcheck during compilation" OFF)
option(DPOR_ENABLE_IWYU "Run include-what-you-use during compilation" OFF)

# ---------- clang-tidy -------------------------------------------------------
if(DPOR_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy)
  if(CLANG_TIDY_EXE)
    # This project uses C++20 structured binding captures which require
    # clang-tidy >= 16 (its internal clang parser must support the feature).
    execute_process(
      COMMAND ${CLANG_TIDY_EXE} --version
      OUTPUT_VARIABLE _clang_tidy_version_output
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REGEX MATCH "[0-9]+" _clang_tidy_major "${_clang_tidy_version_output}")
    if(_clang_tidy_major AND _clang_tidy_major LESS 16)
      message(WARNING
        "clang-tidy ${_clang_tidy_major} found but >= 16 is required for C++20 "
        "structured binding captures. Skipping clang-tidy integration. "
        "Install clang-tidy 16+ or run via the dev container."
      )
    else()
      message(STATUS "clang-tidy found: ${CLANG_TIDY_EXE} (version ${_clang_tidy_major})")
      set(DPOR_CLANG_TIDY_COMMAND "${CLANG_TIDY_EXE}")
    endif()
  else()
    message(WARNING "DPOR_ENABLE_CLANG_TIDY is ON but clang-tidy was not found")
  endif()
endif()

# ---------- cppcheck ---------------------------------------------------------
if(DPOR_ENABLE_CPPCHECK)
  find_program(CPPCHECK_EXE NAMES cppcheck)
  if(CPPCHECK_EXE)
    message(STATUS "cppcheck found: ${CPPCHECK_EXE}")
    set(DPOR_CPPCHECK_COMMAND
      "${CPPCHECK_EXE}"
      "--enable=warning,style,performance,portability"
      "--inconclusive"
      "--suppress=missingIncludeSystem"
      "--suppress=*:${CMAKE_BINARY_DIR}/_deps/*"
      "--suppress=*:*/catch2/*"
      # Suppress inconclusive false positives for trivially-copyable types
      # (size_t, uint32_t, etc.) that cppcheck incorrectly suggests passing
      # by const reference.
      "--suppress=passedByValue"
      # Suppress noisy style findings that conflict with project conventions.
      "--suppress=shadowFunction"
      "--suppress=noExplicitConstructor"
      "--suppress=missingMemberCopy"
      "--suppress=functionStatic"
      "--suppress=unreadVariable"
      "--suppress=useStlAlgorithm"
      "--suppress=unusedPrivateFunction"
      "--suppress=iterateByValue"
      "--inline-suppr"
      "--std=c++20"
      "--quiet"
      "--error-exitcode=1"
    )
  else()
    message(WARNING "DPOR_ENABLE_CPPCHECK is ON but cppcheck was not found")
  endif()
endif()

# ---------- include-what-you-use ---------------------------------------------
if(DPOR_ENABLE_IWYU)
  find_program(IWYU_EXE NAMES include-what-you-use iwyu)
  if(IWYU_EXE)
    message(STATUS "include-what-you-use found: ${IWYU_EXE}")
    set(DPOR_IWYU_COMMAND "${IWYU_EXE}")
  else()
    message(WARNING "DPOR_ENABLE_IWYU is ON but include-what-you-use was not found")
  endif()
endif()

# ---------- helper to apply tools to a target --------------------------------
# Call dpor_apply_static_analysis(target) on every project target.
function(dpor_apply_static_analysis target)
  if(DPOR_CLANG_TIDY_COMMAND)
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${DPOR_CLANG_TIDY_COMMAND}")
  endif()
  if(DPOR_CPPCHECK_COMMAND)
    set_target_properties(${target} PROPERTIES CXX_CPPCHECK "${DPOR_CPPCHECK_COMMAND}")
  endif()
  if(DPOR_IWYU_COMMAND)
    set_target_properties(${target} PROPERTIES CXX_INCLUDE_WHAT_YOU_USE "${DPOR_IWYU_COMMAND}")
  endif()
endfunction()
