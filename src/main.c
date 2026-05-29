#include "qstar/qstar.h"

#include <stdio.h>
#include <string.h>

static void
usage(FILE *out)
{
	fputs("usage: qstar [--file qstar.lua] explain [label]\n", out);
	fputs("       qstar [--file qstar.lua] --dump-graph\n", out);
}

/** 독립 QStar developer binary의 CLI entrypoint다. */
int
main(int argc, char **argv)
{
	struct qstar_graph graph;
	const char *file, *cmd, *label;
	int arg, rc;

	file = "qstar.lua";
	arg = 1;
	while (arg < argc && strcmp(argv[arg], "--file") == 0) {
		if (arg + 1 >= argc) {
			usage(stderr);
			return 2;
		}
		file = argv[arg + 1];
		arg += 2;
	}
	if (arg >= argc) {
		usage(stderr);
		return 2;
	}
	cmd = argv[arg++];
	label = NULL;
	if (strcmp(cmd, "explain") == 0) {
		if (arg < argc)
			label = argv[arg++];
	} else if (strcmp(cmd, "--dump-graph") != 0) {
		usage(stderr);
		return 2;
	}
	if (arg != argc) {
		usage(stderr);
		return 2;
	}
	qstar_graph_init(&graph);
	rc = qstar_lua_eval_file(&graph, file);
	if (rc == 0) {
		if (strcmp(cmd, "explain") == 0)
			rc = qstar_graph_explain_plan(&graph, label, stdout);
		else
			rc = qstar_graph_dump(&graph, label, stdout);
	}
	if (rc < 0) {
		if (graph.error[0])
			fprintf(stderr, "%s\n", graph.error);
		else if (label)
			fprintf(stderr, "qstar: unknown target label '%s'\n", label);
		else
			fprintf(stderr, "qstar: failed\n");
		qstar_graph_free(&graph);
		return 1;
	}
	qstar_graph_free(&graph);
	return 0;
}
