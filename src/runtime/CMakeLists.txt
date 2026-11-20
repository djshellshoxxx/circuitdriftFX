add_library(cd_runtime STATIC Processor.cpp)
target_link_libraries(cd_runtime PUBLIC cd_engine cd_control cd_state)
cd_core_target(cd_runtime)
