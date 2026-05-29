#include "internal.h"

#include <stdio.h>
#include <string.h>

/** public header file이 include/ install surface 아래 있는지 검사한다. */
static int
is_public_header_root(const char *path)
{
	return strncmp(path, "include/", 8) == 0 && path[8] != '\0';
}

/** header list 하나를 public/private install visibility 정책으로 검증한다. */
static int
validate_header_list(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_string_list *list, int public_headers)
{
	const char *path;
	size_t i;

	for (i = 0; i < list->len; i++) {
		path = list->items[i];
		if (!qstar_path_is_package_relative(path))
			return qstar_set_error(graph,
			    "qstar: header path '%s' in '%s' must be package-relative",
			    path, target->label);
		if (public_headers && !is_public_header_root(path))
			return qstar_set_error(graph,
			    "qstar: public header '%s' in '%s' must be under include/",
			    path, target->label);
	}
	return 0;
}

/** QStar header file graph policy를 검증한다. */
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

/** header file 하나의 build-system metadata를 출력한다. */
static void
dump_header(FILE *out, const char *visibility, const char *path)
{
	fprintf(out, "  header_file %s path=%s role=%s semantic=opaque-to-qstar\n",
	    visibility, path, strcmp(visibility, "public") == 0 ? "install" : "internal");
}

/** QStar target의 header file plan skeleton을 출력한다. */
void
qstar_target_dump_header_files(const struct qstar_target *target, FILE *out)
{
	size_t i;

	for (i = 0; i < target->public_headers.len; i++)
		dump_header(out, "public", target->public_headers.items[i]);
	for (i = 0; i < target->private_headers.len; i++)
		dump_header(out, "private", target->private_headers.items[i]);
}
