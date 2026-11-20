# Acquisition and verification boundary for the pinned plugin dependencies.
# Sources and local Git/toolchain metadata must not be modified concurrently with configuration.
include_guard(GLOBAL)
include(FetchContent)
find_package(Git REQUIRED)

# FetchContent_MakeAvailable also runs dependency CMake code, so it cannot be used before
# verification. The fully specified Populate form remains supported with CMP0169 NEW; unlike
# the deprecated single-name form it only acquires content. No provider/find_package hook is
# used for these exact source pins. Existing checkouts are never reset, updated or repaired.
function(cd_acquire_pinned name repo sha submodules)
  string(TOUPPER "${name}" upper_name)
  set(source_dir "${CD_DEPS_CACHE_DIR}/${name}-src")
  set(binary_dir "${CMAKE_BINARY_DIR}/_deps/${name}-build")
  if(NOT "${FETCHCONTENT_SOURCE_DIR_${upper_name}}" STREQUAL "")
    get_filename_component(source_dir "${FETCHCONTENT_SOURCE_DIR_${upper_name}}"
                           ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT IS_DIRECTORY "${source_dir}")
      message(FATAL_ERROR "${name}: source override is not a directory: ${source_dir}")
    endif()
  elseif(NOT EXISTS "${source_dir}")
    if(FETCHCONTENT_FULLY_DISCONNECTED)
      message(FATAL_ERROR "${name}: no source checkout at ${source_dir} while fully disconnected. "
                          "Provide FETCHCONTENT_SOURCE_DIR_${upper_name} or populate the cache first.")
    endif()
    # Older project configurations stored this cache variable in CD_DEPS_CACHE_DIR.
    # Shadow it locally as well as specifying SUBBUILD_DIR: CMP0168 NEW ignores
    # SUBBUILD_DIR and still uses FETCHCONTENT_BASE_DIR for download working files.
    set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")
    FetchContent_Populate(${name}
      GIT_REPOSITORY "${repo}"
      GIT_TAG "${sha}"
      GIT_SUBMODULES "${submodules}"
      GIT_SUBMODULES_RECURSE FALSE
      GIT_PROGRESS TRUE
      SOURCE_DIR "${source_dir}"
      BINARY_DIR "${binary_dir}"
      SUBBUILD_DIR "${CMAKE_BINARY_DIR}/_deps/${name}-subbuild")
  endif()
  set(${name}_SOURCE_DIR "${source_dir}" PARENT_SCOPE)
  set(${name}_BINARY_DIR "${binary_dir}" PARENT_SCOPE)
endfunction()

# Check the actual checkout, not just its base commit. Ignored/untracked files are also
# rejected because CMake can include or glob them. Disable optional index writes and cached
# fsmonitor/untracked answers so verification is read-only and sees current checkout status.
function(cd_verify_pin dir expected_sha label)
  if(NOT IS_DIRECTORY "${dir}" OR NOT EXISTS "${dir}/.git")
    message(FATAL_ERROR "${label}: expected a Git checkout at ${dir}. "
                        "Provide a complete pinned checkout or remove the dependency source cache.")
  endif()
  set(git_command "${GIT_EXECUTABLE}" --no-optional-locks
                  -c core.fsmonitor=false -c core.untrackedCache=false -C "${dir}")
  execute_process(COMMAND ${git_command} rev-parse --show-toplevel
                  OUTPUT_VARIABLE worktree OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_VARIABLE detail RESULT_VARIABLE rc)
  file(REAL_PATH "${dir}" source_real)
  file(REAL_PATH "${worktree}" worktree_real)
  if(WIN32)
    string(TOLOWER "${source_real}" source_real)
    string(TOLOWER "${worktree_real}" worktree_real)
  endif()
  if(NOT rc EQUAL 0 OR NOT source_real STREQUAL worktree_real)
    message(FATAL_ERROR "${label}: Git worktree does not match source directory ${dir}. ${detail}")
  endif()
  execute_process(COMMAND ${git_command} rev-parse HEAD
                  OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_VARIABLE detail RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0 OR NOT actual STREQUAL expected_sha)
    message(FATAL_ERROR "${label}: expected ${expected_sha} but found '${actual}' in ${dir}. "
                        "Provide the pinned checkout or remove the dependency source cache. ${detail}")
  endif()

  # Assume-unchanged and sparse/skip-worktree entries can suppress normal working-tree
  # checks. They are not valid dependency-cache inputs, even when their HEAD matches.
  execute_process(COMMAND ${git_command} ls-files -v
                  OUTPUT_VARIABLE entries ERROR_VARIABLE detail RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0 OR entries MATCHES "(^|\n)[a-zS] ")
    message(FATAL_ERROR "${label}: cannot verify index entries in ${dir}; "
                        "assume-unchanged/skip-worktree entries are unsupported. ${detail}")
  endif()
  execute_process(COMMAND ${git_command} status --porcelain=v1 --untracked-files=all
                          --ignored=matching --ignore-submodules=none
                  OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_VARIABLE detail RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0 OR NOT changes STREQUAL "")
    message(FATAL_ERROR "${label}: source checkout is not clean in ${dir}. "
                        "Tracked, staged, untracked and ignored changes are not accepted. "
                        "Preserve local work separately, then provide a clean pinned checkout. "
                        "${detail}\n${changes}")
  endif()
endfunction()

# This macro intentionally retains the calling directory scope expected by dependency CMake
# APIs. All four checks precede the first add_subdirectory, including on a warm source cache.
macro(cd_add_pinned_plugin_dependencies)
  cd_acquire_pinned(juce "${CD_JUCE_REPO}" "${CD_JUCE_SHA}" "")
  cd_acquire_pinned(clap_juce_extensions "${CD_CJE_REPO}" "${CD_CJE_SHA}"
                    "clap-libs/clap;clap-libs/clap-helpers")
  cd_verify_pin("${juce_SOURCE_DIR}" "${CD_JUCE_SHA}" "JUCE ${CD_JUCE_TAG_NAME}")
  cd_verify_pin("${clap_juce_extensions_SOURCE_DIR}" "${CD_CJE_SHA}" "clap-juce-extensions")
  cd_verify_pin("${clap_juce_extensions_SOURCE_DIR}/clap-libs/clap"
                "${CD_CLAP_SHA}" "CLAP headers")
  cd_verify_pin("${clap_juce_extensions_SOURCE_DIR}/clap-libs/clap-helpers"
                "${CD_CLAP_HELPERS_SHA}" "clap-helpers")
  add_subdirectory("${juce_SOURCE_DIR}" "${juce_BINARY_DIR}")
  add_subdirectory("${clap_juce_extensions_SOURCE_DIR}" "${clap_juce_extensions_BINARY_DIR}")
endmacro()
