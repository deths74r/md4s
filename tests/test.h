/*
 * md4s test framework — minimal, zero-dependency, self-registering.
 *
 * Tests register themselves via __attribute__((constructor)) so you
 * never need to manually maintain a test list. Just write
 * TEST(name) { ... } and it appears in the runner automatically.
 *
 * Usage:
 *
 *     #include "test.h"
 *
 *     TEST(my_feature_works)
 *     {
 *             ASSERT_TRUE(1 + 1 == 2);
 *             ASSERT_EQUAL_INT(4, 2 + 2);
 *             ASSERT_EQUAL_STRING("hello", greeting);
 *     }
 */
#ifndef MD4S_TEST_H
#define MD4S_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_MAX_REGISTERED 1024

struct test_entry {
	const char *test_name;
	void (*function)(void);
};

extern struct test_entry test_registry[TEST_MAX_REGISTERED];
extern int test_registry_count;
extern int test_current_failed;

#define TEST(name)                                                          \
	static void test_body_##name(void);                                 \
	__attribute__((constructor)) static void test_register_##name(void) \
	{                                                                   \
		test_registry[test_registry_count].test_name = #name;        \
		test_registry[test_registry_count].function =               \
			test_body_##name;                                   \
		test_registry_count++;                                      \
	}                                                                   \
	static void test_body_##name(void)

#define TEST_FAIL(format, ...)                                              \
	do {                                                                \
		test_current_failed = 1;                                    \
		fprintf(stderr, "  FAIL %s:%d: " format "\n",              \
			__FILE__, __LINE__, ##__VA_ARGS__);                 \
	} while (0)

#define ASSERT_TRUE(expression)                                             \
	do {                                                                \
		if (!(expression))                                          \
			TEST_FAIL("expected true: %s", #expression);        \
	} while (0)

#define ASSERT_FALSE(expression)                                            \
	do {                                                                \
		if (expression)                                             \
			TEST_FAIL("expected false: %s", #expression);       \
	} while (0)

#define ASSERT_EQUAL_INT(expected, actual)                                   \
	do {                                                                \
		long long _expected = (long long)(expected);                \
		long long _actual = (long long)(actual);                    \
		if (_expected != _actual)                                   \
			TEST_FAIL("expected %lld, got %lld",               \
				  _expected, _actual);                       \
	} while (0)

#define ASSERT_EQUAL_STRING(expected, actual)                                \
	do {                                                                \
		const char *_expected = (expected);                         \
		const char *_actual = (actual);                             \
		if (_expected == NULL && _actual == NULL)                    \
			break;                                              \
		if (_expected == NULL || _actual == NULL ||                  \
		    strcmp(_expected, _actual) != 0)                         \
			TEST_FAIL("expected \"%s\", got \"%s\"",           \
				  _expected ? _expected : "(null)",          \
				  _actual ? _actual : "(null)");             \
	} while (0)

#define ASSERT_NOT_NULL(pointer)                                            \
	do {                                                                \
		if ((pointer) == NULL)                                      \
			TEST_FAIL("expected non-NULL: %s", #pointer);       \
	} while (0)

#define ASSERT_NULL(pointer)                                                \
	do {                                                                \
		if ((pointer) != NULL)                                      \
			TEST_FAIL("expected NULL: %s", #pointer);           \
	} while (0)

#endif /* MD4S_TEST_H */
