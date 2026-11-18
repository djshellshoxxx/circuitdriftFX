# cd_state: versioned documents/checkpoints; worker thread only. Depends on cd_dsp (model catalogue).
add_library(cd_state STATIC StateDocument.cpp)
target_link_libraries(cd_state PUBLIC cd_dsp)
cd_core_target(cd_state)
