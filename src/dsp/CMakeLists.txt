# cd_dsp: portable math core; depends only on cd_core.
add_library(cd_dsp STATIC ThermalNetwork.cpp OutputProtection.cpp Halfband2x.cpp Oversampling.cpp
  DeviceManifest.cpp MnaSolver.cpp)
target_link_libraries(cd_dsp PUBLIC cd_core)
cd_core_target(cd_dsp)
