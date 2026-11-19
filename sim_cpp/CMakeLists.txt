cmake_minimum_required(VERSION 3.16)

project(burst_sim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(BSIM_BUILD_TESTS "Build the unit test binary" ON)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(bsim_core STATIC
  src/rng.cpp
  src/burst_detector.cpp
  src/pi_controller.cpp
  src/adaptation_engine.cpp
  src/model.cpp
  src/simulator.cpp
)
target_include_directories(bsim_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(bsim_core PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# CLI executable
# ---------------------------------------------------------------------------
add_executable(bsim src/main.cpp)
target_link_libraries(bsim PRIVATE bsim_core)

if(BSIM_BUILD_TESTS)
  # -------------------------------------------------------------------------
  # Unit tests
  # -------------------------------------------------------------------------
  enable_testing()
  add_executable(bsim_tests
    tests/test_main.cpp
    tests/test_rng.cpp
    tests/test_burst_detector.cpp
    tests/test_pi_controller.cpp
    tests/test_adaptation_engine.cpp
    tests/test_buffer.cpp
    tests/test_model.cpp
    tests/test_simulator.cpp
  )
  target_link_libraries(bsim_tests PRIVATE bsim_core)
  target_compile_options(bsim_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME bsim_tests COMMAND bsim_tests)
endif()