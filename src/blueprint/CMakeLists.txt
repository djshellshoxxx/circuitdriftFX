# Recovered from PR #10 (71a2637); worker-side graph compilation and bounded
# timeline/fault primitives. UI/runtime installation remains a separate contract.
find_package(Threads REQUIRED)
add_library(cd_blueprint STATIC BlueprintGraph.cpp Compiler.cpp Timeline.cpp Faults.cpp ValidationWorker.cpp)
target_link_libraries(cd_blueprint PUBLIC cd_dsp Threads::Threads)
cd_core_target(cd_blueprint)
