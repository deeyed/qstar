#include "internal.h"

#include <stdio.h>
#include <string.h>

static int
has_suffix(const char *s, const char *suffix)
{
	size_t ns, nfix;

	ns = strlen(s);
	nfix = strlen(suffix);
	return ns >= nfix && strcmp(s + ns - nfix, suffix) == 0;
}

/** header path가 package-relative normalized path인지 검사한다. */
static int
valid_relative_path(const char *path)
{
	const char *p;

	if (!path || !*path || path[0] == '/')
		return 0;
	for (p = path; *p; p++) {
		if ((p == path || p[-1] == '/') && p[0] == '.' &&
		    (p[1] == '/' || p[1] == '\0' ||
		    (p[1] == '.' && (p[2] == '/' || p[2] == '\0'))))
			return 0;
		if (p[0] == '/' && p[1] == '/')
			return 0;
	}
	return 1;
}

/** public header가 include/ public surface 아래 있는지 검사한다. */
static int
is_public_header_root(const char *path)
{
	return strncmp(path, "include/", 8) == 0 && path[8] != '\0';
}

/** header extension을 QStar v1 policy name으로 분류한다. */
static const char *
header_kind(const char *path)
{
	if (has_suffix(path, ".hcl"))
		return "hcl";
	if (has_suffix(path, ".h"))
		return "legacy-h";
	return NULL;
}

/** header list 하나를 public/private visibility 정책으로 검증한다. */
static int
validate_header_list(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_string_list *list, int public_headers)
{
	const char *path;
	size_t i;

	for (i = 0; i < list->len; i++) {
		path = list->items[i];
		if (!valid_relative_path(path))
			return qstar_set_error(graph,
			    "qstar: header path '%s' in '%s' must be package-relative",
			    path, target->label);
		if (!header_kind(path))
			return qstar_set_error(graph,
			    "qstar: unsupported header extension '%s' in '%s'",
			    path, target->label);
		if (public_headers && !is_public_header_root(path))
			return qstar_set_error(graph,
			    "qstar: public header '%s' in '%s' must be under include/",
			    path, target->label);
	}
	return 0;
}

/** QStar header graph policy를 검증한다. */
int
qstar_graph_validate_headers(struct qstar_graph *graph)
{
	size_t i;

	for (i = 0; i < graph->len; i++) {
		if (validate_header_list(graph, &graph->targets[i],
		    &graph->targets[i].public_headers, 1) < 0 ||
		    validate_header_list(graph, &graph->targets[i],
		    &graph->targets[i].private_headers, 0) < 0)
			return -1;
	}
	return 0;
}

/** header 하나의 import/export checker skeleton policy를 출력한다. */
static void
dump_header(FILE *out, const char *visibility, const char *path)
{
	const char *kind;

	kind = header_kind(path);
	fprintf(out, "  header_policy %s path=%s kind=%s ", visibility, path, kind);
	if (strcmp(kind, "hcl") == 0) {
		if (strcmp(visibility, "public") == 0)
			fputs("include=smart export=filtered abi_gate=c-compatible\n", out);
		else
			fputs("include=smart export=private abi_gate=internal\n", out);
	} else {
		if (strcmp(visibility, "public") == 0)
			fputs("include=legacy-textual export=legacy abi_gate=c-header\n", out);
		else
			fputs("include=legacy-textual export=private abi_gate=internal\n", out);
	}
}

/** QStar target의 header import/export policy skeleton을 출력한다. */
void
qstar_target_dump_header_policy(const struct qstar_target *target, FILE *out)
{
	size_t i;

	for (i = 0; i < target->public_headers.len; i++)
		dump_header(out, "public", target->public_headers.items[i]);
	for (i = 0; i < target->private_headers.len; i++)
		dump_header(out, "private", target->private_headers.items[i]);
}
