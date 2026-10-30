cmake_minimum_required(VERSION 3.20)
project(CircuitDrift VERSION 0.0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(CD_SANITIZE "ASan+UBSan for tests" OFF)

# cd_core: IDs, units, parameter registry. No JUCE/UI/network dependency.
add_library(cd_core STATIC src/core/ParameterRegistry.cpp src/core/Json.cpp)
target_include_directories(cd_core PUBLIC src)
# cd_dsp: portable math core; depends only on cd_core.
add_library(cd_dsp STATIC src/dsp/ThermalNetwork.cpp src/dsp/OutputProtection.cpp
  src/dsp/Oversampling.cpp src/dsp/DeviceManifest.cpp src/dsp/MnaSolver.cpp)
target_link_libraries(cd_dsp PUBLIC cd_core)
foreach(t cd_core cd_dsp)
  target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic)
  if(CD_SANITIZE)
    target_compile_options(${t} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${t} PUBLIC -fsanitize=address,undefined)
  endif()
endforeach()

enable_testing()
add_executable(cd_tests tests/test_main.cpp)
target_link_libraries(cd_tests PRIVATE cd_dsp)
add_test(NAME acceptance_core COMMAND cd_tests)
add_test(NAME registry_codegen_fresh COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_param_registry.py --check)
add_test(NAME docs_validation_only COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/validate_planned_features.py)
