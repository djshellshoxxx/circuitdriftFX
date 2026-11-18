// Minimal test helpers shared by the tests/state suites (no framework dependency).
#pragma once
#include <cstdio>
#include <string>

namespace cdtest {
inline int& failures() { static int n = 0; return n; }
}  // namespace cdtest

#define CHECK(c) do { if (!(c)) { ++cdtest::failures(); std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_MSG(c, m) do { if (!(c)) { ++cdtest::failures(); std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #c, std::string(m).c_str()); } } while (0)
#define TEST_RESULT(name) (std::printf("%s: %s\n", name, cdtest::failures() ? "FAILED" : "ok"), cdtest::failures() ? 1 : 0)
