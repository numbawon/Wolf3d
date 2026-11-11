# Wolf3d — Phase 1: data formats (C++23, MSVC arm64)
cmake_minimum_required(VERSION 3.29)
project(wolf3d LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(W3D_BUILD_TESTS "Build the golden test suite" ON)

# Game data lives outside the repo; tests read it at run time.
set(W3D_GAME_DIR
    "C:/Program Files (x86)/Steam/steamapps/common/Wolfenstein 3D"
    CACHE PATH "Root of the Wolfenstein 3D Steam install (base/ + base/m1)")

add_library(w3d_data STATIC
    src/w3d/carmack.cpp
    src/w3d/maps.cpp
    src/w3d/vswap.cpp
    src/w3d/audio.cpp)
target_include_directories(w3d_data PUBLIC src)
target_compile_features(w3d_data PUBLIC cxx_std_23)
if(MSVC)
  target_compile_options(w3d_data PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
  target_compile_options(w3d_data PRIVATE -Wall -Wextra)
endif()

if(W3D_BUILD_TESTS)
  enable_testing()
  add_executable(w3d_tests tests/test_formats.cpp)
  target_link_libraries(w3d_tests PRIVATE w3d_data)
  target_compile_definitions(w3d_tests PRIVATE
      W3D_GAME_DIR_DEFAULT="${W3D_GAME_DIR}")
  if(MSVC)
    target_compile_options(w3d_tests PRIVATE /W4 /permissive- /utf-8)
  endif()
  add_test(NAME formats COMMAND w3d_tests)
endif()
