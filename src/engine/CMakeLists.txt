# cd_engine: integrated circuit runtime (P03). Depends on cd_dsp only; no UI/JUCE/services.
add_library(cd_engine STATIC Circuit.cpp Supply.cpp ComponentTable.cpp CircuitEngine.cpp)
target_link_libraries(cd_engine PUBLIC cd_dsp)
cd_core_target(cd_engine)
