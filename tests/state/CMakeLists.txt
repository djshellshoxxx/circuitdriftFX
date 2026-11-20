cd_add_test(cd_state_queue_tests TEST_NAME state_queue SOURCES test_queue.cpp LIBS cd_state Threads::Threads)
cd_add_test(cd_state_history_tests TEST_NAME state_history SOURCES test_history.cpp LIBS cd_state)
cd_add_test(cd_state_preset_tests TEST_NAME state_preset SOURCES test_preset.cpp LIBS cd_state)
