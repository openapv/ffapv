cmake_minimum_required(VERSION 3.14)
project(ffapvTests)

# GoogleTest requires at least C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(
  oapv_tests
  oapv_tests.cpp
)
target_link_libraries(
  oapv_tests
  GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(oapv_tests)
