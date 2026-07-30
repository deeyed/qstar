#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t allocation_count;
static size_t fail_at;
static size_t live_allocations;

static void *
test_malloc(size_t size)
{
	void *value;

	allocation_count++;
	if (fail_at && allocation_count == fail_at)
		return NULL;
	value = malloc(size);
	if (value)
		live_allocations++;
	return value;
}

static void *
test_realloc(void *old, size_t size)
{
	void *value;

	allocation_count++;
	if (fail_at && allocation_count == fail_at)
		return NULL;
	value = realloc(old, size);
	if (value && !old)
		live_allocations++;
	return value;
}

static void
test_free(void *value)
{
	if (value) {
		if (live_allocations == 0) {
			fputs("qstar-argv-failure-unit: allocation accounting underflow\n",
			    stderr);
			exit(1);
		}
		live_allocations--;
	}
	free(value);
}

#define malloc test_malloc
#define realloc test_realloc
#define free test_free
#include "../src/argv.c"
#undef malloc
#undef realloc
#undef free

static void
fail(const char *message)
{
	fprintf(stderr, "qstar-argv-failure-unit: %s\n", message);
	exit(1);
}

static void
seed(struct qstar_argv *argv)
{
	size_t i;
	char value[32];

	qstar_argv_init(argv);
	for (i = 0; i < 8; i++) {
		snprintf(value, sizeof(value), "seed-%zu", i);
		if (qstar_argv_push(argv, value) < 0)
			fail("could not seed argv");
	}
}

static void
exercise_push_failure(size_t failure_offset)
{
	struct qstar_argv argv;
	size_t old_len, old_cap, old_bytes, baseline;

	fail_at = 0;
	seed(&argv);
	old_len = argv.len;
	old_cap = argv.cap;
	old_bytes = argv.bytes;
	baseline = allocation_count;
	fail_at = baseline + failure_offset;
	if (qstar_argv_push(&argv, "growth-trigger") == 0)
		fail("injected push unexpectedly succeeded");
	if (argv.len != old_len || argv.cap != old_cap ||
	    argv.bytes != old_bytes || argv.items[argv.len] != NULL)
		fail("failed push changed the original argv");
	fail_at = 0;
	qstar_argv_free(&argv);
	if (live_allocations != 0)
		fail("failed push leaked allocations");
}

static void
exercise_clone_failure(size_t failure_offset)
{
	struct qstar_argv src, dst;
	size_t baseline;

	fail_at = 0;
	seed(&src);
	qstar_argv_init(&dst);
	if (qstar_argv_push(&dst, "destination") < 0)
		fail("could not seed clone destination");
	baseline = allocation_count;
	fail_at = baseline + failure_offset;
	if (qstar_argv_clone(&dst, &src) == 0)
		fail("injected clone unexpectedly succeeded");
	if (dst.len != 1 || strcmp(dst.items[0], "destination") != 0)
		fail("failed clone changed the destination");
	fail_at = 0;
	qstar_argv_free(&dst);
	qstar_argv_free(&src);
	if (live_allocations != 0)
		fail("failed clone leaked allocations");
}

int
main(void)
{
	size_t offset;

	for (offset = 1; offset <= 2; offset++)
		exercise_push_failure(offset);
	for (offset = 1; offset <= 9; offset++)
		exercise_clone_failure(offset);
	puts("qstar-argv-failure-unit: passed");
	return 0;
}
