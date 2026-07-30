#include "internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int
qstar_argv_init(struct qstar_argv *argv)
{
	if (!argv)
		return -1;
	memset(argv, 0, sizeof(*argv));
	return 0;
}

int
qstar_argv_reserve(struct qstar_argv *argv, size_t count)
{
	char **items;
	size_t cap, slots;

	if (!argv)
		return -1;
	if (count <= argv->cap) {
		if (argv->items)
			argv->items[argv->len] = NULL;
		return 0;
	}
	if (count == SIZE_MAX)
		return -1;
	cap = argv->cap ? argv->cap : 8;
	while (cap < count) {
		if (cap > SIZE_MAX / 2) {
			cap = count;
			break;
		}
		cap *= 2;
	}
	if (cap == SIZE_MAX)
		return -1;
	slots = cap + 1;
	if (slots > SIZE_MAX / sizeof(*items))
		return -1;
	items = realloc(argv->items, slots * sizeof(*items));
	if (!items)
		return -1;
	argv->items = items;
	argv->cap = cap;
	argv->items[argv->len] = NULL;
	return 0;
}

int
qstar_argv_push(struct qstar_argv *argv, const char *value)
{
	char *copy;
	size_t n;

	if (!argv || !value)
		return -1;
	n = strlen(value);
	if (argv->len == SIZE_MAX || n == SIZE_MAX ||
	    argv->bytes > SIZE_MAX - (n + 1))
		return -1;
	copy = malloc(n + 1);
	if (!copy)
		return -1;
	memcpy(copy, value, n + 1);
	if (qstar_argv_reserve(argv, argv->len + 1) < 0) {
		free(copy);
		return -1;
	}
	argv->items[argv->len++] = copy;
	argv->items[argv->len] = NULL;
	argv->bytes += n + 1;
	return 0;
}

int
qstar_argv_pushf(struct qstar_argv *argv, const char *fmt, ...)
{
	va_list ap, copy_ap;
	char *value;
	int n, written;
	int rc;

	if (!argv || !fmt)
		return -1;
	va_start(ap, fmt);
	va_copy(copy_ap, ap);
	n = vsnprintf(NULL, 0, fmt, copy_ap);
	va_end(copy_ap);
	if (n < 0 || (size_t)n == SIZE_MAX) {
		va_end(ap);
		return -1;
	}
	value = malloc((size_t)n + 1);
	if (!value) {
		va_end(ap);
		return -1;
	}
	written = vsnprintf(value, (size_t)n + 1, fmt, ap);
	va_end(ap);
	if (written != n) {
		free(value);
		return -1;
	}
	rc = qstar_argv_push(argv, value);
	free(value);
	return rc;
}

int
qstar_argv_clone(struct qstar_argv *dst, const struct qstar_argv *src)
{
	struct qstar_argv copy;
	size_t i;

	if (!dst || !src)
		return -1;
	qstar_argv_init(&copy);
	if (qstar_argv_reserve(&copy, src->len) < 0)
		return -1;
	for (i = 0; i < src->len; i++) {
		if (qstar_argv_push(&copy, src->items[i]) < 0) {
			qstar_argv_free(&copy);
			return -1;
		}
	}
	qstar_argv_free(dst);
	*dst = copy;
	return 0;
}

char *const *
qstar_argv_data(struct qstar_argv *argv)
{
	static char *empty[] = {NULL};

	return argv && argv->items ? argv->items : empty;
}

void
qstar_argv_free(struct qstar_argv *argv)
{
	size_t i;

	if (!argv)
		return;
	for (i = 0; i < argv->len; i++)
		free(argv->items[i]);
	free(argv->items);
	memset(argv, 0, sizeof(*argv));
}
