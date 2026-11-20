cmake_minimum_required(VERSION 3.22)
project(CircuitDrift VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(CD_SANITIZE "ASan+UBSan for tests (GCC/Clang only)" OFF)
option(CD_BUILD_TESTS "Build framework-independent core tests" ON)
option(CD_BUILD_PLUGIN "Build JUCE plugin targets (fetches pinned JUCE/CLAP adapter)" OFF)

# Shared warning/sanitizer settings for framework-independent core targets.
include(cmake/CoreTarget.cmake)
if(CD_BUILD_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)
endif()

# Dependency direction (CLAUDE.md): cd_core <- cd_dsp <- cd_engine/cd_control/cd_state/cd_blueprint
# <- cd_runtime (framework-independent integration) <- plugin adapters/UI.
# Each module owns its CMakeLists.txt; modules that do not exist yet are skipped.
foreach(dir core dsp engine control state blueprint media network community runtime)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/${dir}/CMakeLists.txt)
    add_subdirectory(src/${dir})
  endif()
endforeach()

if(CD_BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(CD_BUILD_PLUGIN AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/plugin/CMakeLists.txt)
  add_subdirectory(plugin)
endif()
