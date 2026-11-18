# Audio-thread trap harness (test-only). Own suite; Linux/glibc only; deliberately NOT sanitized:
# ASan/TSan own malloc/operator new, so this executable gets its own unsanitized copies of the
# cd_core/cd_dsp sources instead of linking the (possibly sanitized) cd_* libraries. The trap
# object is never linked into cd_core/cd_dsp/cd_state or any other test.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  foreach(lib core dsp)
    get_target_property(_dir cd_${lib} SOURCE_DIR)
    get_target_property(_src cd_${lib} SOURCES)
    set(_abs "")
    foreach(s ${_src})
      list(APPEND _abs ${_dir}/${s})
    endforeach()
    add_library(cd_${lib}_rt STATIC ${_abs})
    target_include_directories(cd_${lib}_rt PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(cd_${lib}_rt PRIVATE -Wall -Wextra -Wpedantic)
  endforeach()
  target_link_libraries(cd_dsp_rt PUBLIC cd_core_rt)

  add_executable(cd_rt_trap_tests ${CMAKE_SOURCE_DIR}/tests/test_rt_trap.cpp
                 ${CMAKE_CURRENT_SOURCE_DIR}/rt_trap.cpp)
  target_include_directories(cd_rt_trap_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_compile_options(cd_rt_trap_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(cd_rt_trap_tests PRIVATE cd_dsp_rt Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME rt_trap COMMAND cd_rt_trap_tests)
endif()
