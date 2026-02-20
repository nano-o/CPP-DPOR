# dpor

A C++20 DPOR model-checking library inspired by Must.
See the Must paper in docs/.

## Goals

- Keep a small stable API for consumers.
- Make algorithm internals easy to evolve.
- Ship as a reusable CMake package (`dpor::dpor`).

## Current model assumptions

- Receives are modeled as blocking only.
- Non-blocking receive behavior is currently out of scope.

## Build

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Tests are written with Catch2 (v3). CMake uses a system Catch2 package if available.

If Catch2 is not installed and your environment has internet access, enable fetching:

```bash
cmake --preset debug-fetch-catch2
cmake --build --preset debug-fetch-catch2
ctest --preset debug-fetch-catch2
```

## Editor setup (clangd)

All presets export `compile_commands.json`. Symlink it to the project root so clangd can find it:

```bash
ln -sf build/debug/compile_commands.json .
```

## Install

```bash
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix /tmp/dpor-install
```

## Consume from another CMake project

```cmake
find_package(dpor CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE dpor::dpor)
```

If installed to a non-default prefix, add it to `CMAKE_PREFIX_PATH`.

## Layout

- `include/dpor/api`: public, stable headers
- `src`: implementation details
- `tests`: unit/integration tests with CTest
- `examples`: minimal usage examples
- `cmake`: packaging and build helper modules
- `docs`: architecture notes
