# cd_control: macros, creative drift, tempo breathing, key/scale tracking, pitch detector,
# harmonic targeting, transient recovery, sidechain load. Depends on cd_dsp (and cd_core) only.
add_library(cd_control STATIC
  MacroMapper.cpp CreativeDrift.cpp TempoBreathing.cpp ScaleQuantizer.cpp KeyTracker.cpp
  PitchDetector.cpp HarmonicTargeting.cpp TransientDetector.cpp SidechainLoad.cpp ControlProcessor.cpp
  ControlSnapshot.cpp)
target_link_libraries(cd_control PUBLIC cd_dsp)
cd_core_target(cd_control)
