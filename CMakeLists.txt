# Wolf3d — C++23, MSVC arm64 (data layer, game sim, render)
cmake_minimum_required(VERSION 3.29)
project(wolf3d LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(W3D_BUILD_TESTS "Build the golden test suite" ON)
option(W3D_BUILD_RENDER "Build the Phase 3 renderer (needs SDL3 + Vulkan deps)" ON)

# Game data lives outside the repo; tests read it at run time.
set(W3D_GAME_DIR
    "C:/Program Files (x86)/Steam/steamapps/common/Wolfenstein 3D"
    CACHE PATH "Root of the Wolfenstein 3D Steam install (base/ + base/m1)")

# Pinned build dependencies (SDL3, Vulkan headers) live in the gitignored
# .build/deps; cmake/fetch_deps.py re-creates them if missing.
if(W3D_BUILD_RENDER)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  execute_process(
      COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/cmake/fetch_deps.py"
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      RESULT_VARIABLE W3D_DEPS_RC)
  if(NOT W3D_DEPS_RC EQUAL 0)
    message(FATAL_ERROR "w3d: dependency fetch failed (see above)")
  endif()
  set(W3D_DEPS_DIR "${CMAKE_SOURCE_DIR}/.build/deps")
endif()

add_library(w3d_data STATIC
    src/w3d/carmack.cpp
    src/w3d/maps.cpp
    src/w3d/vswap.cpp
    src/w3d/vga.cpp
    src/w3d/audio.cpp
    src/w3d/opl2.cpp
    src/w3d/imf.cpp)
target_include_directories(w3d_data PUBLIC src)
target_compile_features(w3d_data PUBLIC cxx_std_23)
if(MSVC)
  target_compile_options(w3d_data PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
  target_compile_options(w3d_data PRIVATE -Wall -Wextra)
endif()

# Phase 2: headless game simulation on the data layer.
add_library(w3d_game STATIC
    src/game/actors.cpp
    src/game/input.cpp
    src/game/level.cpp
    src/game/sim.cpp)
target_link_libraries(w3d_game PUBLIC w3d_data)
target_include_directories(w3d_game PUBLIC src)
if(MSVC)
  target_compile_options(w3d_game PRIVATE /W4 /permissive- /utf-8)
endif()

# Phase 4: audio (OPL2 synth + IMF sequencer + resampling/positional mixer).
# The mixer is pure C++ (no device dependency) so it is fully testable.
add_library(w3d_audio STATIC
    src/audio/mixer.cpp)
target_link_libraries(w3d_audio PUBLIC w3d_data)
target_include_directories(w3d_audio PUBLIC src)
if(MSVC)
  target_compile_options(w3d_audio PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(w3d_audio PRIVATE -Wall -Wextra)
endif()

# Phase 5: UI — scene state machine + full-res menu model (WS2). Pure C++
# (no SDL / Vulkan) so it is testable headless; the renderer consumes
# app/menu.hpp (POD, std430-matched) and app/font8x16.inc.
add_library(w3d_appui STATIC
    src/app/scene.cpp
    src/app/menu.cpp)
target_include_directories(w3d_appui PUBLIC src)
if(MSVC)
  target_compile_options(w3d_appui PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(w3d_appui PRIVATE -Wall -Wextra)
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

  add_executable(w3d_sim_tests tests/test_sim.cpp)
  target_link_libraries(w3d_sim_tests PRIVATE w3d_game)
  target_compile_definitions(w3d_sim_tests PRIVATE
      W3D_GAME_DIR_DEFAULT="${W3D_GAME_DIR}")
  if(MSVC)
    target_compile_options(w3d_sim_tests PRIVATE /W4 /permissive- /utf-8)
  endif()
  add_test(NAME sim COMMAND w3d_sim_tests)

  add_executable(w3d_audio_tests tests/test_audio.cpp)
  target_link_libraries(w3d_audio_tests PRIVATE w3d_audio)
  target_compile_definitions(w3d_audio_tests PRIVATE
      W3D_GAME_DIR_DEFAULT="${W3D_GAME_DIR}")
  if(MSVC)
    target_compile_options(w3d_audio_tests PRIVATE /W4 /permissive- /utf-8)
  endif()
  add_test(NAME audio COMMAND w3d_audio_tests)

  add_executable(w3d_scene_tests tests/test_scene.cpp)
  target_link_libraries(w3d_scene_tests PRIVATE w3d_appui)
  if(MSVC)
    target_compile_options(w3d_scene_tests PRIVATE /W4 /permissive- /utf-8)
  endif()
  add_test(NAME scene COMMAND w3d_scene_tests)
endif()

# Phase 4: audio evidence tool — renders the game's music / FM effects / a
# PCM SFX to WAV files at the target rate. Doubles as the demonstration that
# the resampling stage accepts any input rate/width (see --pcm44k).
add_executable(w3d_tool_audio_dump tools/w3d_audio_dump.cpp)
target_link_libraries(w3d_tool_audio_dump PRIVATE w3d_audio)
target_compile_definitions(w3d_tool_audio_dump PRIVATE
    W3D_GAME_DIR_DEFAULT="${W3D_GAME_DIR}")
if(MSVC)
  target_compile_options(w3d_tool_audio_dump PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(w3d_tool_audio_dump PRIVATE -Wall -Wextra)
endif()

# Phase 3: renderer.
# Links the pinned arm64 Vulkan loader (built from source in .build/deps);
# the system's System32 vulkan-1.dll is x64 and would force Prism
# emulation of the whole client.
if(W3D_BUILD_RENDER)
  set(W3D_VULKAN_LOADER "${W3D_DEPS_DIR}/vulkan_loader/vulkan-1.dll")
  set(W3D_VULKAN_LOADER_LIB "${W3D_DEPS_DIR}/vulkan_loader/vulkan-1.lib")
  if(NOT EXISTS "${W3D_VULKAN_LOADER}")
    message(FATAL_ERROR "w3d: ${W3D_VULKAN_LOADER} missing (deps not fetched?)")
  endif()
  if(NOT EXISTS "${W3D_VULKAN_LOADER_LIB}")
    message(FATAL_ERROR "w3d: ${W3D_VULKAN_LOADER_LIB} missing (run .build\\build_loader.bat)")
  endif()
  set(W3D_SDL3_DIR "${W3D_DEPS_DIR}/sdl3")
  # The SDL3 release ships only the DLL; build_arm64.bat generates the import
  # library from its export table before configure.
  set(W3D_SDL3_LIB "${W3D_SDL3_DIR}/SDL3.lib")
  if(NOT EXISTS "${W3D_SDL3_LIB}")
    message(FATAL_ERROR "w3d: ${W3D_SDL3_LIB} missing (run .build\\build_arm64.bat)")
  endif()

  add_library(w3d_render STATIC
      src/render/frame.cpp
      src/render/renderer.cpp
      src/render/window.cpp
      src/util/bmp.cpp)
  target_include_directories(w3d_render
      PUBLIC src
      SYSTEM PUBLIC "${W3D_SDL3_DIR}"
      SYSTEM PUBLIC "${W3D_DEPS_DIR}/vulkan")
  target_link_libraries(w3d_render PUBLIC w3d_game
      "${W3D_SDL3_LIB}" "${W3D_VULKAN_LOADER_LIB}")
  target_compile_definitions(w3d_render PUBLIC
      VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(w3d_render PRIVATE /W4 /permissive- /utf-8)
  endif()

  # GLSL -> SPIR-V at build time (glslang is an x86_64 prebuilt that runs
  # under Prism; SPIR-V itself is portable).
  set(W3D_GLSLANG "${W3D_DEPS_DIR}/glslang/bin/glslang.exe")
  function(w3d_add_shaders target)
    set(spvs "")
    foreach(sh IN LISTS ARGN)
      get_filename_component(base ${sh} NAME_WE)
      set(spv "${CMAKE_BINARY_DIR}/generated/shaders/${base}.spv")
      add_custom_command(
          OUTPUT ${spv}
          COMMAND ${W3D_GLSLANG} -V --target-env vulkan1.4 -O
                  "${CMAKE_SOURCE_DIR}/src/render/shaders/${sh}" -o "${spv}"
          DEPENDS "${CMAKE_SOURCE_DIR}/src/render/shaders/${sh}"
          COMMENT "glslang ${sh} -> ${base}.spv"
          VERBATIM)
      list(APPEND spvs ${spv})
    endforeach()
    add_custom_target(${target}_shaders DEPENDS ${spvs})
    add_dependencies(${target} ${target}_shaders)
    # Copy the .spv files next to the executable. This must be a stamp-based
    # custom command, NOT a POST_BUILD step: POST_BUILD only runs when the
    # target relinks, so a shader-only edit would leave stale .spv files in
    # the runtime directory (the app loads from there).
    set(shader_stamp "${CMAKE_BINARY_DIR}/generated/shaders/${target}_copied")
    add_custom_command(
        OUTPUT ${shader_stamp}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/generated/shaders"
            "$<TARGET_FILE_DIR:${target}>/shaders"
        COMMAND ${CMAKE_COMMAND} -E touch ${shader_stamp}
        DEPENDS ${spvs}
        COMMENT "copy ${target} shaders to runtime dir"
        VERBATIM)
    add_custom_target(${target}_shader_copy DEPENDS ${shader_stamp})
    add_dependencies(${target} ${target}_shader_copy)
  endfunction()

  # Game application (M3: simulation + raycast renderer).
  add_executable(w3d_app src/main.cpp)
  target_link_libraries(w3d_app PRIVATE w3d_render w3d_game w3d_appui)
  w3d_add_shaders(w3d_app walls.comp sprites.comp menu.comp)
  if(MSVC)
    target_compile_options(w3d_app PRIVATE /W4 /permissive- /utf-8)
  endif()
  add_custom_command(TARGET w3d_app POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${W3D_SDL3_DIR}/SDL3.dll" "$<TARGET_FILE_DIR:w3d_app>"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${W3D_VULKAN_LOADER}" "$<TARGET_FILE_DIR:w3d_app>")

  # Driver/toolchain probe (milestone 1).
  add_executable(w3d_tool_vulkan_probe tools/vulkan_probe.cpp)
  target_include_directories(w3d_tool_vulkan_probe SYSTEM PRIVATE
      "${W3D_DEPS_DIR}/vulkan")
  target_link_libraries(w3d_tool_vulkan_probe PRIVATE "${W3D_VULKAN_LOADER_LIB}")
  add_custom_command(TARGET w3d_tool_vulkan_probe POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${W3D_VULKAN_LOADER}" "$<TARGET_FILE_DIR:w3d_tool_vulkan_probe>")
  if(MSVC)
    target_compile_options(w3d_tool_vulkan_probe PRIVATE /W4 /permissive- /utf-8)
  endif()
endif()
