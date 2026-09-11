// The whole test framework: three macros and a counter.
//
// Shared so that the panel tests and the input tests report into one total and
// one exit code, without dragging in a dependency. The counters are defined in
// test_panel.cpp, which also owns main().
#pragma once
#include <cmath>
#include <cstdio>

extern int g_failures;
extern int g_checks;
extern const char* g_case;

#define CASE(name) g_case = name
#define CHECK(cond)                                                             \
  do {                                                                          \
    ++g_checks;                                                                 \
    if (!(cond)) {                                                              \
      ++g_failures;                                                             \
      std::printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
    }                                                                           \
  } while (0)
#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    ++g_checks;                                                            \
    const long _a = static_cast<long>(a), _b = static_cast<long>(b);       \
    if (_a != _b) {                                                        \
      ++g_failures;                                                        \
      std::printf("  FAIL %s:%d [%s] %s == %s (%ld vs %ld)\n", __FILE__,   \
                  __LINE__, g_case, #a, #b, _a, _b);                       \
    }                                                                      \
  } while (0)
#define CHECK_NEAR(a, b, tol)                                              \
  do {                                                                     \
    ++g_checks;                                                            \
    const double _a = (a), _b = (b);                                       \
    if (std::fabs(_a - _b) > (tol)) {                                      \
      ++g_failures;                                                        \
      std::printf("  FAIL %s:%d [%s] %s ~= %s (%.3f vs %.3f)\n", __FILE__, \
                  __LINE__, g_case, #a, #b, _a, _b);                       \
    }                                                                      \
  } while (0)

// Defined in test_ui.cpp, called from main() in test_panel.cpp.
void run_ui_tests();
