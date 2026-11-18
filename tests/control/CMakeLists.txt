# cd_control acceptance suites (T06, T12-T16 control-level parts). Synthetic fixtures only (G01 open).
foreach(suite macros key pitch tempo harmonic dynamics)
  cd_add_test(cd_control_${suite}_tests TEST_NAME control_${suite} SOURCES test_${suite}.cpp LIBS cd_control)
endforeach()
target_link_libraries(cd_control_pitch_tests PRIVATE Threads::Threads)
