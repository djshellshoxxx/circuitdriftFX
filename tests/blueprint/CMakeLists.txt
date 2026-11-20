foreach(suite compile timeline faults validation_worker)
  cd_add_test(cd_blueprint_${suite}_tests TEST_NAME blueprint_${suite}
    SOURCES test_${suite}.cpp LIBS cd_blueprint)
endforeach()
target_link_libraries(cd_blueprint_compile_tests PRIVATE Threads::Threads)
