# cd_engine suites (P03). Deterministic; synthetic fixtures only (G01 open).
cd_add_test(cd_engine_devices_tests TEST_NAME engine_devices SOURCES test_engine_devices.cpp LIBS cd_engine)
cd_add_test(cd_engine_runtime_tests TEST_NAME engine_runtime SOURCES test_engine_runtime.cpp LIBS cd_engine)
# T21 on the integrated circuit; runs its independent measurements in parallel test threads.
cd_add_test(cd_engine_t21_tests TEST_NAME engine_t21 SOURCES test_engine_t21.cpp LIBS cd_engine Threads::Threads)
# Stress (finite/bounded under hostile automation) and the ungated ns/sample benchmark.
cd_add_test(cd_engine_stress_tests TEST_NAME engine_stress SOURCES test_engine_stress.cpp LIBS cd_engine Threads::Threads)
