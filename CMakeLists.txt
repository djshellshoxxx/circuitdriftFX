cmake_minimum_required(VERSION 3.20)
project(CircuitDrift VERSION 0.0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(CD_SANITIZE "ASan+UBSan for tests" OFF)

# Module sources are globbed per directory so independent modules can be added without
# editing this file. Dependency direction (CLAUDE.md): core <- dsp <- control/state <- blueprint <- runtime.
function(cd_module name dir)
  file(GLOB _src CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/${dir}/*.cpp)
  add_library(${name} STATIC ${_src})
  target_include_directories(${name} PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(${name} PUBLIC ${ARGN})
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  if(CD_SANITIZE)
    target_compile_options(${name} PUBLIC -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer)
    target_link_options(${name} PUBLIC -fsanitize=address,undefined,float-cast-overflow)
  endif()
endfunction()

find_package(Threads REQUIRED)
# cd_core: IDs, units, parameter registry, JSON. No JUCE/UI/network dependency.
cd_module(cd_core core)
# cd_dsp: portable math core.
cd_module(cd_dsp dsp cd_core)
# cd_control: macros, detectors, tempo/key, creative motion (P05/P06). Audio-safe evaluators.
cd_module(cd_control control cd_dsp)
# cd_state: versioned documents, checkpoints, A/B/history, transaction queues (P04/P07).
cd_module(cd_state state cd_dsp Threads::Threads)
# cd_blueprint: graph validator, lifecycle timeline, faults (P10/P11).
cd_module(cd_blueprint blueprint cd_state cd_control)
# cd_runtime: integrated per-sample engine (P03 integration) and realtime bridge.
cd_module(cd_runtime runtime cd_blueprint cd_control cd_state)

enable_testing()
add_executable(cd_tests tests/test_main.cpp)
target_link_libraries(cd_tests PRIVATE cd_state)
add_test(NAME acceptance_core COMMAND cd_tests)
# Every other tests/test_<suite>.cpp becomes its own executable/test linked against all modules.
file(GLOB _suites CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
list(REMOVE_ITEM _suites ${CMAKE_SOURCE_DIR}/tests/test_main.cpp)
file(GLOB _support CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/support/*.cpp)
foreach(f ${_suites})
  get_filename_component(stem ${f} NAME_WE)
  string(REGEX REPLACE "^test_" "" suite ${stem})
  add_executable(cd_${suite}_tests ${f} ${_support})
  target_include_directories(cd_${suite}_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(cd_${suite}_tests PRIVATE cd_runtime Threads::Threads)
  add_test(NAME ${suite} COMMAND cd_${suite}_tests)
endforeach()
add_test(NAME registry_codegen_fresh COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_param_registry.py --check)
add_test(NAME docs_validation_only COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/validate_planned_features.py)
