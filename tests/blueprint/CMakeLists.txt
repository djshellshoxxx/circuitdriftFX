foreach(suite compile timeline faults)
  cd_add_test(cd_blueprint_${suite}_tests TEST_NAME blueprint_${suite}
    SOURCES test_${suite}.cpp LIBS cd_blueprint)
endforeach()
