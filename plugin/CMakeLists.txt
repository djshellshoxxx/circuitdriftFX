# CircuitDrift plugin products (full + LITE): VST3, CLAP and Standalone on Windows; the same
# targets configure on Linux for developer smoke builds. AU (macOS only) and AAX (gate G08) are
# out of scope for this release line, see docs/adr/ADR-0001-build-formats-and-ids.md.
#
# Enable with: cmake -S . -B build-plugin -DCD_BUILD_PLUGIN=ON
# The top-level project must call add_subdirectory(plugin) and enable_testing() before it.

if(CMAKE_VERSION VERSION_LESS 3.25)
  message(FATAL_ERROR "The plugin build needs CMake >= 3.25 (JUCE 8); found ${CMAKE_VERSION}")
endif()

# Distributable Windows binaries use the static MSVC runtime so users need no VC++ redistributable.
# The runtime must match across cd_core/cd_dsp/cd_state, the tests and the plugins (LNK2038
# otherwise), so it has to be chosen when configuring the whole tree:
#   -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>"
if(MSVC)
  if(NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded" OR CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
    message(FATAL_ERROR
      "Configure the plugin build with the static MSVC runtime:\n"
      "  -DCMAKE_MSVC_RUNTIME_LIBRARY=\"MultiThreaded$<$<CONFIG:Debug>:Debug>\"\n"
      "(current value: '${CMAKE_MSVC_RUNTIME_LIBRARY}')")
  endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/Dependencies.cmake")

# Shared adapter code: parameter layout built from the core registry and the processor shell.
# INTERFACE library: JUCE modules are themselves INTERFACE targets whose sources compile into each
# consumer, so a STATIC wrapper would compile JUCE twice (ODR/duplicate symbols, non-PIC objects).
# Each product and the test app therefore compile these adapter sources and JUCE exactly once.
add_library(cd_plugin_shared INTERFACE)
# Adapter + UI sources. UI areas (shell, workbench, analysis, panels, platform) are globbed so each
# area owner adds files without editing this list; CONFIGURE_DEPENDS re-globs on build.
file(GLOB_RECURSE CD_PLUGIN_UI_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/*.cpp)
target_sources(cd_plugin_shared INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ParameterLayout.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/PluginProcessor.cpp
  ${CD_PLUGIN_UI_SOURCES})
target_include_directories(cd_plugin_shared INTERFACE src)
# Framework-independent core libraries that exist in this tree (dependency direction per CLAUDE.md).
set(CD_PLUGIN_CORE_LIBS cd_core cd_dsp cd_state)
foreach(lib cd_engine cd_control cd_blueprint cd_media cd_network cd_community cd_runtime)
  if(TARGET ${lib})
    list(APPEND CD_PLUGIN_CORE_LIBS ${lib})
  endif()
endforeach()
target_link_libraries(cd_plugin_shared INTERFACE
  ${CD_PLUGIN_CORE_LIBS}
  juce::juce_audio_utils)
target_compile_features(cd_plugin_shared INTERFACE cxx_std_20)
target_compile_definitions(cd_plugin_shared INTERFACE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_DISPLAY_SPLASH_SCREEN=0
  JUCE_REPORT_APP_USAGE=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_STRICT_REFCOUNTEDPOINTER=1
  JUCE_MODAL_LOOPS_PERMITTED=0)

# Adds one product: JUCE plugin (VST3 + Standalone) plus the CLAP wrapper.
function(cd_add_product target product_name plugin_code bundle_id clap_id entry_source)
  juce_add_plugin(${target}
    PRODUCT_NAME "${product_name}"
    COMPANY_NAME "CircuitDrift"
    BUNDLE_ID "${bundle_id}"
    PLUGIN_MANUFACTURER_CODE Cdrf
    PLUGIN_CODE ${plugin_code}
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Distortion
    DESCRIPTION "${product_name} - analog circuit drift distortion")
  target_sources(${target} PRIVATE ${entry_source})
  target_link_libraries(${target}
    PRIVATE cd_plugin_shared juce::juce_recommended_warning_flags
    PUBLIC juce::juce_recommended_config_flags)
  clap_juce_extensions_plugin(TARGET ${target}
    CLAP_ID "${clap_id}"
    CLAP_FEATURES audio-effect distortion stereo)
endfunction()

# Permanent identities (never change after release): PLUGIN_CODE, BUNDLE_ID and CLAP_ID.
cd_add_product(CircuitDrift "CircuitDrift" Cdfx
  "com.circuitdrift.circuitdrift" "com.circuitdrift.circuitdrift" src/PluginEntryFull.cpp)
cd_add_product(CircuitDriftLite "CircuitDrift LITE" Cdlt
  "com.circuitdrift.circuitdrift.lite" "com.circuitdrift.circuitdrift.lite" src/PluginEntryLite.cpp)

# Consistency tests: JUCE parameter tree vs core registry, golden host-id table, bus layouts.
juce_add_console_app(cd_plugin_consistency_tests PRODUCT_NAME "cd_plugin_consistency_tests")
target_sources(cd_plugin_consistency_tests PRIVATE tests/test_plugin_params.cpp)
target_link_libraries(cd_plugin_consistency_tests PRIVATE cd_plugin_shared
  juce::juce_recommended_config_flags)
add_test(NAME plugin_param_consistency COMMAND cd_plugin_consistency_tests)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME plugin_tables_fresh
           COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_plugin_tables.py" --check)
endif()

# UI snapshot apps: each plugin/tests/ui/snap_<area>.cpp renders its views headlessly (both themes,
# several scales) to PNG under the build tree and checks layout invariants. Registered with CTest.
file(GLOB CD_UI_SNAPSHOT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/snap_*.cpp)
foreach(src ${CD_UI_SNAPSHOT_SOURCES})
  get_filename_component(area ${src} NAME_WE)
  juce_add_console_app(cd_ui_${area} PRODUCT_NAME "cd_ui_${area}")
  target_sources(cd_ui_${area} PRIVATE ${src})
  target_include_directories(cd_ui_${area} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui)
  target_link_libraries(cd_ui_${area} PRIVATE cd_plugin_shared juce::juce_recommended_config_flags)
  add_test(NAME ui_${area} COMMAND cd_ui_${area} ${CMAKE_CURRENT_BINARY_DIR}/ui-snapshots)
endforeach()
