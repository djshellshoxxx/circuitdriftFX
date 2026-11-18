# Pinned third-party sources for the plugin build (see docs/adr/ADR-0001-build-formats-and-ids.md).
#
# Every dependency is pinned to a full commit SHA. A git commit id is content-addressed, so the
# pin is cryptographically strong without a separate archive hash; after checkout the SHA is
# re-verified below. Changing any value here is a deliberate dependency update (ADR + notices).
#
# Only the downloaded sources and the FetchContent sub-build stamps live under CD_DEPS_CACHE_DIR
# (cacheable in CI keyed on hashFiles('cmake/Dependencies.cmake')); build trees stay in the build dir.

include_guard(GLOBAL)
include(FetchContent)

set(CD_DEPS_CACHE_DIR "${CMAKE_BINARY_DIR}/_deps_cache" CACHE PATH
    "Directory holding fetched dependency sources (safe to cache between CI runs)")
set(FETCHCONTENT_BASE_DIR "${CD_DEPS_CACHE_DIR}" CACHE PATH "" FORCE)

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

find_package(Git REQUIRED)

# Declares one pinned git dependency with sources in the cache dir and the build tree in the build dir.
function(cd_declare_pinned name repo sha submodules)
  FetchContent_Declare(${name}
    GIT_REPOSITORY "${repo}"
    GIT_TAG "${sha}"
    GIT_SUBMODULES "${submodules}"
    GIT_PROGRESS TRUE
    SOURCE_DIR "${CD_DEPS_CACHE_DIR}/${name}-src"
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/${name}-build")
endfunction()

# Fails configuration if the checked-out HEAD of a source directory is not the pinned SHA.
function(cd_verify_pin dir expected_sha label)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse HEAD
                  OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0 OR NOT actual STREQUAL expected_sha)
    message(FATAL_ERROR "${label}: expected ${expected_sha} but found '${actual}' in ${dir}. "
                        "Delete ${CD_DEPS_CACHE_DIR} and reconfigure.")
  endif()
endfunction()

cd_declare_pinned(juce "${CD_JUCE_REPO}" "${CD_JUCE_SHA}" "")
cd_declare_pinned(clap_juce_extensions "${CD_CJE_REPO}" "${CD_CJE_SHA}" "clap-libs/clap clap-libs/clap-helpers")

# JUCE first: clap-juce-extensions looks for the juce:: targets when it is added.
FetchContent_MakeAvailable(juce)
cd_verify_pin("${juce_SOURCE_DIR}" "${CD_JUCE_SHA}" "JUCE ${CD_JUCE_TAG_NAME}")
FetchContent_MakeAvailable(clap_juce_extensions)
cd_verify_pin("${clap_juce_extensions_SOURCE_DIR}" "${CD_CJE_SHA}" "clap-juce-extensions")
cd_verify_pin("${clap_juce_extensions_SOURCE_DIR}/clap-libs/clap" "${CD_CLAP_SHA}" "CLAP headers")
cd_verify_pin("${clap_juce_extensions_SOURCE_DIR}/clap-libs/clap-helpers" "${CD_CLAP_HELPERS_SHA}" "clap-helpers")

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
