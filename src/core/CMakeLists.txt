# cd_core: IDs, units, parameter registry, strict JSON. No JUCE/UI/network dependency.
add_library(cd_core STATIC ParameterRegistry.cpp Json.cpp)
target_include_directories(cd_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
cd_core_target(cd_core)
