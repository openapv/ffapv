# System tests for OpenAPV in ffmpeg

This directory contains comprehensive performance benchmark tests for the OpenAPV codec integrated into FFmpeg. The test suite measures encoding and decoding performance across various resolutions and provides quality metrics.

- **Decoding Performance**: Comparison between native FFmpeg decoding and OpenAPV decoding
- **Multiple Resolutions**: Tests from 240p to 8K resolution

## Prerequisites

Before building and running the tests, ensure you have the following dependencies installed:

- **CMake** (version 3.14 or later)
- **C++17 compatible compiler** (GCC 7+, Clang 5+, or MSVC 2017+)
- **FFmpeg** with liboapv codec support
- **Git** (for fetching GoogleTest)


# Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
cd build
ctest
```

### Run Specific Test Cases

```bash
# Run all benchmark tests
./oapv_tests

# Run specific resolution test
./oapv_tests --gtest_filter="*apv_1080p*"

# Run with report
./oapv_tests --gtest_output=json:test_results.json
```
