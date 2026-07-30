#include "internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
fail(const char *message)
{
	fprintf(stderr, "qstar-argv-unit: %s\n", message);
	exit(1);
}

static void
expect_shape(const struct qstar_argv *argv, size_t len, size_t bytes)
{
	if (argv->len != len)
		fail("unexpected argc");
	if (argv->bytes != bytes)
		fail("unexpected byte count");
	if (argv->items && argv->items[argv->len] != NULL)
		fail("argv is not NULL terminated");
}

static void
exercise_count(size_t count)
{
	struct qstar_argv argv;
	char value[64];
	size_t i, bytes;

	if (qstar_argv_init(&argv) < 0)
		fail("init failed");
	if (!qstar_argv_data(&argv) || qstar_argv_data(&argv)[0] != NULL)
		fail("empty argv is not NULL terminated");
	bytes = 0;
	for (i = 0; i < count; i++) {
		snprintf(value, sizeof(value), "atom-%zu", i);
		if (qstar_argv_push(&argv, value) < 0)
			fail("push failed");
		bytes += strlen(value) + 1;
		expect_shape(&argv, i + 1, bytes);
	}
	if (count && strcmp(argv.items[count - 1], value) != 0)
		fail("last atom changed");
	qstar_argv_free(&argv);
	qstar_argv_free(&argv);
	expect_shape(&argv, 0, 0);
}

int
main(void)
{
	static const size_t counts[] = {0, 1, 255, 256, 1000, 4096};
	struct qstar_argv argv, clone;
	char **data;
	size_t i, old_len, old_cap, old_bytes;

	for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
		exercise_count(counts[i]);
	if (qstar_argv_init(&argv) < 0 || qstar_argv_init(&clone) < 0)
		fail("init failed");
	if (qstar_argv_push(&argv, "") < 0 ||
	    qstar_argv_push(&argv, "two words") < 0 ||
	    qstar_argv_push(&argv, "quote\"and'apostrophe") < 0 ||
	    qstar_argv_pushf(&argv, "formatted-%d", 42) < 0)
		fail("special atom push failed");
	expect_shape(&argv, 4,
	    1 + strlen("two words") + 1 +
	    strlen("quote\"and'apostrophe") + 1 +
	    strlen("formatted-42") + 1);
	data = (char **)qstar_argv_data(&argv);
	if (!data || data[argv.len] != NULL)
		fail("data is not NULL terminated");
	if (qstar_argv_clone(&clone, &argv) < 0)
		fail("clone failed");
	if (clone.len != argv.len || clone.bytes != argv.bytes ||
	    clone.items == argv.items || clone.items[1] == argv.items[1])
		fail("clone is not independent");
	clone.items[1][0] = 'T';
	if (strcmp(argv.items[1], "two words") != 0)
		fail("clone mutation affected source");
	old_len = argv.len;
	old_cap = argv.cap;
	old_bytes = argv.bytes;
	if (qstar_argv_reserve(&argv, SIZE_MAX) == 0)
		fail("overflowing reserve succeeded");
	if (argv.len != old_len || argv.cap != old_cap || argv.bytes != old_bytes ||
	    argv.items[argv.len] != NULL)
		fail("overflowing reserve changed argv");
	argv.len = SIZE_MAX;
	if (qstar_argv_push(&argv, "overflow") == 0)
		fail("overflowing push succeeded");
	argv.len = old_len;
	if (argv.cap != old_cap || argv.bytes != old_bytes ||
	    argv.items[argv.len] != NULL)
		fail("overflowing push changed argv");
	qstar_argv_free(&clone);
	qstar_argv_free(&argv);
	puts("qstar-argv-unit: passed");
	return 0;
}
