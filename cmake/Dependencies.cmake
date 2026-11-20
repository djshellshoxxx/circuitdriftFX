# Pinned third-party sources for the plugin build (see docs/adr/ADR-0001-build-formats-and-ids.md).
#
# Every dependency is pinned to a full commit SHA. Both the revision and clean source contents
# are checked before any dependency CMake code runs. Changing a pin is a deliberate dependency
# update (ADR + notices); a matching HEAD alone does not validate an edited working tree.
#
# Only source checkouts live under CD_DEPS_CACHE_DIR. Population scripts/stamps and all build
# trees stay in the current build directory and must not be restored from a source cache.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/PinnedDependencies.cmake")

set(CD_DEPS_CACHE_DIR "${CMAKE_BINARY_DIR}/_deps_cache" CACHE PATH
    "Directory holding pinned dependency source checkouts (verified before configuration)")

# --- Pins -------------------------------------------------------------------------------------
# JUCE 8.0.17-security (dual licence AGPLv3 / JUCE 8 EULA; the owner must choose, see ADR-0001).
set(CD_JUCE_REPO "https://github.com/juce-framework/JUCE.git")
set(CD_JUCE_TAG_NAME "8.0.17-security")
set(CD_JUCE_SHA "dabd5ad13cd869cba41046947fffc8e5ce5cd662")
# clap-juce-extensions (MIT). Its submodules clap-libs/clap and clap-libs/clap-helpers resolve to
# the SHAs recorded in this exact commit; they are re-verified below.
set(CD_CJE_REPO "https://github.com/free-audio/clap-juce-extensions.git")
set(CD_CJE_SHA "3837f4a6ed2ab0ba46aba69ae5b7041c7c64ce74")
set(CD_CLAP_SHA "195b42a004144fab0b3cf95e9c067187d15365b7")
set(CD_CLAP_HELPERS_SHA "a380c978f429103e08afc1d17e9efbf3e3c96ee2")

# --- JUCE configuration ---------------------------------------------------------------------
# JUCE reads these as CMake options/definitions when its modules are compiled.
set(JUCE_ENABLE_MODULE_SOURCE_GROUPS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Acquire without adding either project, verify both roots and required submodules, then
# configure JUCE before clap-juce-extensions (which consumes the juce:: targets).
cd_add_pinned_plugin_dependencies()

# Informational audit of how the pinned wrappers derive host parameter ids (ADR-0001, section 3).
set(CD_JUCE_VST3_WRAPPER
    "${juce_SOURCE_DIR}/modules/juce_audio_plugin_client/VST3/juce_VST3_Wrapper.cpp")
if(EXISTS "${CD_JUCE_VST3_WRAPPER}")
  file(STRINGS "${CD_JUCE_VST3_WRAPPER}" cd_vst3_id_lines REGEX "hashCode|getIntValue|FORCE_USE_LEGACY_PARAM_IDS")
  list(LENGTH cd_vst3_id_lines cd_vst3_id_count)
  message(STATUS "ID audit: JUCE VST3 wrapper has ${cd_vst3_id_count} id-derivation lines "
                 "(hashCode / legacy-id); see ADR-0001")
  if(cd_vst3_id_count EQUAL 0)
    message(WARNING "ID audit: expected hashCode/legacy-id derivation not found in the pinned JUCE VST3 wrapper; "
                    "re-run the audit in ADR-0001 before releasing.")
  endif()
endif()
