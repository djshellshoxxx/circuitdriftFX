# cd_state: versioned documents/checkpoints, parameter transaction queue, A/B ops, undo history and
# local preset library. Worker/message thread only, except the header-only queue consumer/publisher.
# Depends on cd_dsp (model catalogue).
find_package(Threads REQUIRED)
add_library(cd_state STATIC
  StateDocument.cpp
  Config.cpp
  ConfigDelta.cpp
  History.cpp
  Preset.cpp
  FactoryPresets.cpp
  PresetLibrary.cpp
  PresetCatalogWorker.cpp)
target_link_libraries(cd_state PUBLIC cd_dsp Threads::Threads)
cd_core_target(cd_state)
