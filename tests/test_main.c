/*
 * md4s test runner. Iterates through all self-registered tests, runs
 * each one, and prints a summary.
 */
#include "test.h"

struct test_entry test_registry[TEST_MAX_REGISTERED];
int test_registry_count = 0;
int test_current_failed = 0;

int main(void)
{
	int passed = 0;
	int failed = 0;

	printf("Running %d tests...\n\n", test_registry_count);

	for (int i = 0; i < test_registry_count; i++) {
		test_current_failed = 0;

		printf("  %-60s ", test_registry[i].test_name);
		fflush(stdout);

		test_registry[i].function();

		if (test_current_failed) {
			printf("FAIL\n");
			failed++;
		} else {
			printf("ok\n");
			passed++;
		}
	}

	printf("\n%d passed, %d failed, %d total\n",
	       passed, failed, test_registry_count);

	return failed > 0 ? 1 : 0;
}
