#include "qstar/qstar.h"

#include <stdio.h>
#include <string.h>

static void
usage(FILE *out)
{
	fputs("usage: qstar [options] explain [label]\n", out);
	fputs("       qstar [options] --dump-graph\n", out);
	fputs("options:\n", out);
	fputs("       --file qstar.lua\n", out);
	fputs("       --package-alias @name=/path\n", out);
	fputs("       --profile name --target triple --toolchain name --stdlib policy\n", out);
}

/** CLI package alias spec을 alias/root 쌍으로 분리해 graph에 기록한다. */
static int
add_package_alias_spec(struct qstar_graph *graph, const char *spec)
{
	const char *eq;
	char alias[QSTAR_PATH_MAX];
	size_t n;

	eq = strchr(spec, '=');
	if (!eq || eq == spec || !eq[1])
		return qstar_graph_add_package_alias(graph, "", "");
	n = (size_t)(eq - spec);
	if (n + 1 > sizeof(alias))
		return qstar_graph_add_package_alias(graph, "", "");
	memcpy(alias, spec, n);
	alias[n] = '\0';
	return qstar_graph_add_package_alias(graph, alias, eq + 1);
}

/** 독립 QStar developer binary의 CLI entrypoint다. */
int
main(int argc, char **argv)
{
	struct qstar_graph graph;
	const char *file, *cmd, *label;
	int arg, rc;

	qstar_graph_init(&graph);
	file = "qstar.lua";
	arg = 1;
	while (arg < argc && strncmp(argv[arg], "--", 2) == 0 &&
	    strcmp(argv[arg], "--dump-graph") != 0) {
		if (strcmp(argv[arg], "--file") == 0) {
			if (arg + 1 >= argc) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
			file = argv[arg + 1];
			arg += 2;
		} else if (strcmp(argv[arg], "--package-alias") == 0) {
			if (arg + 1 >= argc) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
			if (add_package_alias_spec(&graph, argv[arg + 1]) < 0) {
				fprintf(stderr, "%s\n", graph.error);
				qstar_graph_free(&graph);
				return 1;
			}
			arg += 2;
		} else if (strcmp(argv[arg], "--profile") == 0 ||
		    strcmp(argv[arg], "--target") == 0 ||
		    strcmp(argv[arg], "--toolchain") == 0 ||
		    strcmp(argv[arg], "--stdlib") == 0) {
			if (arg + 1 >= argc) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
			if (strcmp(argv[arg], "--profile") == 0)
				rc = qstar_graph_set_profile_input(&graph, argv[arg + 1], NULL, NULL, NULL);
			else if (strcmp(argv[arg], "--target") == 0)
				rc = qstar_graph_set_profile_input(&graph, NULL, argv[arg + 1], NULL, NULL);
			else if (strcmp(argv[arg], "--toolchain") == 0)
				rc = qstar_graph_set_profile_input(&graph, NULL, NULL, argv[arg + 1], NULL);
			else
				rc = qstar_graph_set_profile_input(&graph, NULL, NULL, NULL, argv[arg + 1]);
			if (rc < 0) {
				fprintf(stderr, "%s\n", graph.error);
				qstar_graph_free(&graph);
				return 1;
			}
			arg += 2;
		} else {
			usage(stderr);
			qstar_graph_free(&graph);
			return 2;
		}
	}
	if (arg >= argc) {
		usage(stderr);
		qstar_graph_free(&graph);
		return 2;
	}
	cmd = argv[arg++];
	label = NULL;
	if (strcmp(cmd, "explain") == 0) {
		if (arg < argc)
			label = argv[arg++];
	} else if (strcmp(cmd, "--dump-graph") != 0) {
		usage(stderr);
		qstar_graph_free(&graph);
		return 2;
	}
	if (arg != argc) {
		usage(stderr);
		qstar_graph_free(&graph);
		return 2;
	}
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
