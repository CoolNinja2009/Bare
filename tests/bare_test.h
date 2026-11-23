/* bare_test.h: a tiny, dependency-free test runner.
 *
 *     TEST(adds) { CHECK(1 + 1 == 2); CHECK_NEAR(0.1f + 0.2f, 0.3f, 1e-6); }
 *     int main(void) { RUN(adds); return test_report(); }
 *
 * A failed CHECK prints file:line and the expression, then leaves the test. */
#ifndef BARE_TEST_H
#define BARE_TEST_H

#include <math.h>
#include <stdio.h>

static int test_count, test_failures, test_current_failed;
static const char *test_current;

#define TEST(name) static void name(void)

#define CHECK(cond) do { \
    if (!(cond)) { test_fail_(__FILE__, __LINE__, #cond); return; } \
} while (0)

#define CHECK_NEAR(a, b, eps) do { \
    double a_ = (double)(a), b_ = (double)(b); \
    if (!(fabs(a_ - b_) <= (eps))) { \
        char msg_[256]; \
        snprintf(msg_, sizeof msg_, "%s == %s (got %g, expected %g)", #a, #b, a_, b_); \
        test_fail_(__FILE__, __LINE__, msg_); return; \
    } \
} while (0)

#define RUN(name) test_run_(#name, name)

static void test_fail_(const char *file, int line, const char *what) {
    printf("  FAIL %s\n       %s:%d: %s\n", test_current, file, line, what);
    test_current_failed = 1;
}

static void test_run_(const char *name, void (*fn)(void)) {
    test_current = name;
    test_current_failed = 0;
    fn();
    test_count++;
    if (test_current_failed) test_failures++;
    else printf("  ok   %s\n", name);
}

static int test_report(void) {
    printf("%d/%d passed\n", test_count - test_failures, test_count);
    return test_failures ? 1 : 0;
}

#endif
