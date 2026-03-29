/**
 * DBSQL Test Framework
 *
 * Modern C-based testing infrastructure for comprehensive code coverage.
 * Features:
 * - TAP (Test Anything Protocol) output
 * - Memory leak detection
 * - Coverage reporting integration
 * - Performance benchmarking
 * - Sanitizer-friendly
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

/* BSD types needed for Berkeley DB compatibility with -std=c99 */
#ifndef _BSD_SOURCE
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned char u_char;
typedef unsigned short u_short;
#endif

/* BSD timersub macro if not already defined */
#ifndef timersub
#define timersub(a, b, result) \
	do { \
		(result)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
		(result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
		if ((result)->tv_usec < 0) { \
			--(result)->tv_sec; \
			(result)->tv_usec += 1000000; \
		} \
	} while (0)
#endif

/* Test result tracking */
typedef struct {
	int tests_run;
	int tests_passed;
	int tests_failed;
	int tests_skipped;
	struct timeval start_time;
	FILE *tap_output;
} test_context_t;

/* Global test context */
extern test_context_t g_test_ctx;

/* Test macros */
#define TEST_INIT(name) \
	do { \
		memset(&g_test_ctx, 0, sizeof(g_test_ctx)); \
		gettimeofday(&g_test_ctx.start_time, NULL); \
		g_test_ctx.tap_output = stdout; \
		fprintf(g_test_ctx.tap_output, "# Test Suite: %s\n", (name)); \
	} while (0)

#define TEST_FINISH() \
	do { \
		struct timeval end_time, diff; \
		gettimeofday(&end_time, NULL); \
		timersub(&end_time, &g_test_ctx.start_time, &diff); \
		fprintf(g_test_ctx.tap_output, "1..%d\n", g_test_ctx.tests_run); \
		fprintf(g_test_ctx.tap_output, "# Tests: %d\n", g_test_ctx.tests_run); \
		fprintf(g_test_ctx.tap_output, "# Passed: %d\n", g_test_ctx.tests_passed); \
		fprintf(g_test_ctx.tap_output, "# Failed: %d\n", g_test_ctx.tests_failed); \
		fprintf(g_test_ctx.tap_output, "# Skipped: %d\n", g_test_ctx.tests_skipped); \
		fprintf(g_test_ctx.tap_output, "# Time: %ld.%06ld seconds\n", \
			(long)diff.tv_sec, (long)diff.tv_usec); \
		return (g_test_ctx.tests_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE; \
	} while (0)

#define TEST(name) \
	static void test_##name(void); \
	static void __attribute__((constructor)) register_##name(void) { \
		test_register(#name, test_##name); \
	} \
	static void test_##name(void)

#define ASSERT_EQ(expected, actual) \
	do { \
		g_test_ctx.tests_run++; \
		if ((expected) == (actual)) { \
			g_test_ctx.tests_passed++; \
			fprintf(g_test_ctx.tap_output, "ok %d - %s == %s\n", \
				g_test_ctx.tests_run, #expected, #actual); \
		} else { \
			g_test_ctx.tests_failed++; \
			fprintf(g_test_ctx.tap_output, "not ok %d - %s == %s (expected=%ld, actual=%ld)\n", \
				g_test_ctx.tests_run, #expected, #actual, \
				(long)(expected), (long)(actual)); \
		} \
	} while (0)

#define ASSERT_NE(expected, actual) \
	do { \
		g_test_ctx.tests_run++; \
		if ((expected) != (actual)) { \
			g_test_ctx.tests_passed++; \
			fprintf(g_test_ctx.tap_output, "ok %d - %s != %s\n", \
				g_test_ctx.tests_run, #expected, #actual); \
		} else { \
			g_test_ctx.tests_failed++; \
			fprintf(g_test_ctx.tap_output, "not ok %d - %s != %s (both=%ld)\n", \
				g_test_ctx.tests_run, #expected, #actual, (long)(expected)); \
		} \
	} while (0)

#define ASSERT_TRUE(condition) \
	do { \
		g_test_ctx.tests_run++; \
		if (condition) { \
			g_test_ctx.tests_passed++; \
			fprintf(g_test_ctx.tap_output, "ok %d - %s\n", \
				g_test_ctx.tests_run, #condition); \
		} else { \
			g_test_ctx.tests_failed++; \
			fprintf(g_test_ctx.tap_output, "not ok %d - %s\n", \
				g_test_ctx.tests_run, #condition); \
		} \
	} while (0)

#define ASSERT_FALSE(condition) \
	do { \
		g_test_ctx.tests_run++; \
		if (!(condition)) { \
			g_test_ctx.tests_passed++; \
			fprintf(g_test_ctx.tap_output, "ok %d - !(%s)\n", \
				g_test_ctx.tests_run, #condition); \
		} else { \
			g_test_ctx.tests_failed++; \
			fprintf(g_test_ctx.tap_output, "not ok %d - !(%s)\n", \
				g_test_ctx.tests_run, #condition); \
		} \
	} while (0)

#define ASSERT_NULL(pointer) ASSERT_EQ(NULL, pointer)
#define ASSERT_NOT_NULL(pointer) ASSERT_NE(NULL, pointer)

#define ASSERT_STR_EQ(expected, actual) \
	do { \
		g_test_ctx.tests_run++; \
		if (strcmp((expected), (actual)) == 0) { \
			g_test_ctx.tests_passed++; \
			fprintf(g_test_ctx.tap_output, "ok %d - strcmp(%s, %s)\n", \
				g_test_ctx.tests_run, #expected, #actual); \
		} else { \
			g_test_ctx.tests_failed++; \
			fprintf(g_test_ctx.tap_output, "not ok %d - strcmp(%s, %s) (expected=\"%s\", actual=\"%s\")\n", \
				g_test_ctx.tests_run, #expected, #actual, (expected), (actual)); \
		} \
	} while (0)

#define TEST_SKIP(reason) \
	do { \
		g_test_ctx.tests_run++; \
		g_test_ctx.tests_skipped++; \
		fprintf(g_test_ctx.tap_output, "ok %d # SKIP %s\n", \
			g_test_ctx.tests_run, (reason)); \
		return; \
	} while (0)

#define TEST_TODO(reason) \
	fprintf(g_test_ctx.tap_output, "# TODO: %s\n", (reason))

/* Test registration (for automatic test discovery) */
typedef void (*test_func_t)(void);

typedef struct test_entry {
	const char *name;
	test_func_t func;
	struct test_entry *next;
} test_entry_t;

void test_register(const char *name, test_func_t func);
void test_run_all(void);

/* Memory tracking for leak detection */
#ifdef TEST_TRACK_MEMORY
void *test_malloc(size_t size, const char *file, int line);
void *test_calloc(size_t nmemb, size_t size, const char *file, int line);
void *test_realloc(void *ptr, size_t size, const char *file, int line);
void test_free(void *ptr, const char *file, int line);
void test_memory_report(void);

#define malloc(size) test_malloc(size, __FILE__, __LINE__)
#define calloc(nmemb, size) test_calloc(nmemb, size, __FILE__, __LINE__)
#define realloc(ptr, size) test_realloc(ptr, size, __FILE__, __LINE__)
#define free(ptr) test_free(ptr, __FILE__, __LINE__)
#endif

/* Performance benchmarking */
typedef struct {
	struct timeval start;
	const char *name;
} benchmark_t;

#define BENCHMARK_START(name) \
	benchmark_t bench_##name; \
	bench_##name.name = #name; \
	gettimeofday(&bench_##name.start, NULL)

#define BENCHMARK_END(name) \
	do { \
		struct timeval end, diff; \
		gettimeofday(&end, NULL); \
		timersub(&end, &bench_##name.start, &diff); \
		fprintf(g_test_ctx.tap_output, "# BENCHMARK %s: %ld.%06ld seconds\n", \
			bench_##name.name, (long)diff.tv_sec, (long)diff.tv_usec); \
	} while (0)

#endif /* TEST_FRAMEWORK_H */
