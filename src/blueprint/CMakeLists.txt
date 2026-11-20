# Recovered from PR #10 (71a2637); worker-side graph compilation and bounded
# timeline/fault primitives. UI/runtime installation remains a separate contract.
add_library(cd_blueprint STATIC BlueprintGraph.cpp Compiler.cpp Timeline.cpp Faults.cpp)
target_link_libraries(cd_blueprint PUBLIC cd_dsp)
cd_core_target(cd_blueprint)
