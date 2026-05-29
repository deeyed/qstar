#include "qstar/qstar.h"

#include <stdio.h>
#include <string.h>

static void
usage(FILE *out)
{
	fputs("usage: qstar [options] list-targets\n", out);
	fputs("       qstar [options] query [label]\n", out);
	fputs("       qstar [options] doctor\n", out);
	fputs("       qstar [options] check [label]\n", out);
	fputs("       qstar [options] explain [label]\n", out);
	fputs("       qstar [options] dry-run [label]\n", out);
	fputs("       qstar [options] --dump-graph\n", out);
	fputs("options:\n", out);
	fputs("       --file qstar.lua\n", out);
	fputs("       --package-alias @name=/path\n", out);
	fputs("       --profile name --target triple --toolchain name --stdlib policy\n", out);
	fputs("       --diagnostic-format text|line\n", out);
}

/** QStar diagnostic을 text 또는 machine-readable line skeleton으로 출력한다. */
static void
print_error(const struct qstar_graph *graph, const char *label, const char *format)
{
	const char *message;

	message = graph->error[0] ? graph->error :
	    label ? "qstar: unknown target label" : "qstar: failed";
	if (format && strcmp(format, "line") == 0) {
		fprintf(stderr,
		    "qstar-diagnostic-v1 severity=error file=%s line=%d field=%s label=%s message=%s\n",
		    graph->error_file[0] ? graph->error_file : "<unknown>",
		    graph->error_line,
		    graph->error_field[0] ? graph->error_field : "<none>",
		    graph->error_label[0] ? graph->error_label : (label ? label : "<none>"),
		    message);
		return;
	}
	fprintf(stderr, "%s\n", message);
	if (graph->error_file[0])
		fprintf(stderr, "qstar: origin %s:%d field=%s label=%s\n",
		    graph->error_file, graph->error_line,
		    graph->error_field[0] ? graph->error_field : "<none>",
		    graph->error_label[0] ? graph->error_label : (label ? label : "<none>"));
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
	const char *file, *cmd, *label, *diagnostic_format;
	int arg, rc;

	qstar_graph_init(&graph);
	file = "qstar.lua";
	diagnostic_format = "text";
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
		} else if (strcmp(argv[arg], "--diagnostic-format") == 0) {
			if (arg + 1 >= argc ||
			    (strcmp(argv[arg + 1], "text") != 0 && strcmp(argv[arg + 1], "line") != 0)) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
			diagnostic_format = argv[arg + 1];
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
	if (strcmp(cmd, "explain") == 0 || strcmp(cmd, "dry-run") == 0 ||
	    strcmp(cmd, "check") == 0 || strcmp(cmd, "query") == 0) {
		if (arg < argc)
			label = argv[arg++];
	} else if (strcmp(cmd, "--dump-graph") != 0 && strcmp(cmd, "list-targets") != 0 &&
	    strcmp(cmd, "doctor") != 0) {
		usage(stderr);
		qstar_graph_free(&graph);
		return 2;
	}
	if (strcmp(cmd, "query") == 0 && (!label || !*label)) {
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
	if (rc == 0)
		rc = qstar_graph_validate_generated_outputs(&graph);
	if (rc == 0)
		rc = qstar_graph_validate_sources(&graph);
	if (rc == 0)
		rc = qstar_graph_validate_headers(&graph);
	if (rc == 0 && (strcmp(cmd, "check") == 0 || strcmp(cmd, "doctor") == 0))
		rc = qstar_graph_validate_file_inputs(&graph);
	if (rc == 0) {
		if (strcmp(cmd, "explain") == 0)
			rc = qstar_graph_explain_plan(&graph, label, stdout);
		else if (strcmp(cmd, "dry-run") == 0)
			rc = qstar_graph_dry_run(&graph, label, stdout);
		else if (strcmp(cmd, "check") == 0)
			rc = qstar_graph_check(&graph, label, stdout);
		else if (strcmp(cmd, "doctor") == 0)
			rc = qstar_graph_doctor(&graph, stdout);
		else if (strcmp(cmd, "list-targets") == 0)
			rc = qstar_graph_list_targets(&graph, stdout);
		else if (strcmp(cmd, "query") == 0)
			rc = qstar_graph_query(&graph, label, stdout);
		else
			rc = qstar_graph_dump(&graph, label, stdout);
	}
	if (rc < 0) {
		print_error(&graph, label, diagnostic_format);
		qstar_graph_free(&graph);
		return 1;
	}
	qstar_graph_free(&graph);
	return 0;
}
