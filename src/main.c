#include "internal.h"

#include <stdio.h>
#include <string.h>

static void
usage(FILE *out)
{
	fputs("usage: qstar [options] list-targets\n", out);
	fputs("       qstar [options] list-targets --format json\n", out);
	fputs("       qstar [options] query [label]\n", out);
	fputs("       qstar [options] doctor\n", out);
	fputs("       qstar [options] check [label]\n", out);
	fputs("       qstar [options] lint [label|//...] [--format text|json]\n", out);
	fputs("       qstar [options] fmt [--check] [qstar.lua|fragment.qs]\n", out);
	fputs("       qstar [options] explain [label]\n", out);
	fputs("       qstar [options] dry-run [label]\n", out);
	fputs("       qstar [options] build [label]\n", out);
	fputs("       qstar [options] test [label|//...]\n", out);
	fputs("       qstar [options] install [label] --prefix path [--dry-run]\n", out);
	fputs("       qstar [options] stage <label> [--root path] [--dry-run]\n", out);
	fputs("       qstar [options] why-rebuild [label]\n", out);
	fputs("       qstar [options] clean [--target label]\n", out);
	fputs("       qstar [options] log [label]\n", out);
	fputs("       qstar [options] last-failure\n", out);
	fputs("       qstar [options] action-log <action-id>\n", out);
	fputs("       qstar [options] replay <action-id>\n", out);
	fputs("       qstar lsp --stdio\n", out);
	fputs("       qstar init c-app|c-lib|generated|mixed-cale [directory]\n", out);
	fputs("       qstar [options] --dump-graph\n", out);
	fputs("options:\n", out);
	fputs("       --file qstar.lua\n", out);
	fputs("       --package-alias @name=/path\n", out);
	fputs("       --profile name --target triple --toolchain name --stdlib policy\n", out);
	fputs("       --diagnostics text|json\n", out);
	fputs("       --diagnostic-format text|line  # compatibility alias\n", out);
	fputs("build options:\n", out);
	fputs("       --jobs N\n", out);
	fputs("       --schedule-trace\n", out);
}

/** JSON diagnostic string을 stderr에 출력한다. */
static void
print_json_string(const char *s)
{
	const unsigned char *p = (const unsigned char *)(s ? s : "");

	fputc('"', stderr);
	while (*p) {
		if (*p == '"' || *p == '\\')
			fprintf(stderr, "\\%c", *p);
		else if (*p == '\n')
			fputs("\\n", stderr);
		else if (*p == '\r')
			fputs("\\r", stderr);
		else if (*p == '\t')
			fputs("\\t", stderr);
		else
			fputc(*p, stderr);
		p++;
	}
	fputc('"', stderr);
}

/** QStar diagnostic을 text 또는 machine-readable skeleton으로 출력한다. */
static void
print_error(const struct qstar_graph *graph, const char *label, const char *format)
{
	const char *message;

	message = graph->error[0] ? graph->error :
	    label ? "qstar: unknown target label" : "qstar: failed";
	if (format && strcmp(format, "json") == 0) {
		fputs("{\"schema\":\"qstar-diagnostic-v1\",\"severity\":\"error\",\"file\":", stderr);
		print_json_string(graph->error_file[0] ? graph->error_file : "<unknown>");
		fprintf(stderr, ",\"line\":%d,\"field\":", graph->error_line);
		print_json_string(graph->error_field[0] ? graph->error_field : "<none>");
		fputs(",\"label\":", stderr);
		print_json_string(graph->error_label[0] ? graph->error_label :
		    (label ? label : "<none>"));
		fputs(",\"message\":", stderr);
		print_json_string(message);
		fputs("}\n", stderr);
		return;
	}
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
	const char *file, *cmd, *label, *diagnostic_format, *lint_format, *list_format;
	const char *cli_profile, *cli_target, *cli_toolchain, *cli_stdlib;
	struct qstar_build_options build_options;
	struct qstar_install_options install_options;
	struct qstar_stage_options stage_options;
	char init_error[512];
	int arg, rc;

	qstar_graph_init(&graph);
	memset(&build_options, 0, sizeof(build_options));
	memset(&install_options, 0, sizeof(install_options));
	memset(&stage_options, 0, sizeof(stage_options));
	file = "qstar.lua";
	diagnostic_format = "text";
	lint_format = "text";
	list_format = "text";
	cli_profile = NULL;
	cli_target = NULL;
	cli_toolchain = NULL;
	cli_stdlib = NULL;
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
		} else if (strcmp(argv[arg], "--diagnostics") == 0 ||
		    strcmp(argv[arg], "--diagnostic-format") == 0) {
			if (arg + 1 >= argc ||
			    (strcmp(argv[arg + 1], "text") != 0 &&
			    strcmp(argv[arg + 1], "line") != 0 &&
			    strcmp(argv[arg + 1], "json") != 0)) {
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
				cli_profile = argv[arg + 1];
			else if (strcmp(argv[arg], "--target") == 0)
				cli_target = argv[arg + 1];
			else if (strcmp(argv[arg], "--toolchain") == 0)
				cli_toolchain = argv[arg + 1];
			else
				cli_stdlib = argv[arg + 1];
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
	build_options.jobs = 1;
	if (strcmp(cmd, "init") == 0) {
		const char *template_name, *directory;

		if (arg >= argc) {
			usage(stderr);
			qstar_graph_free(&graph);
			return 2;
		}
		template_name = argv[arg++];
		directory = ".";
		if (arg < argc)
			directory = argv[arg++];
		if (arg != argc) {
			usage(stderr);
			qstar_graph_free(&graph);
			return 2;
		}
		init_error[0] = '\0';
		rc = qstar_init_project(template_name, directory, stdout, init_error,
		    sizeof(init_error));
		if (rc < 0) {
			fprintf(stderr, "%s\n", init_error[0] ? init_error :
			    "qstar: init failed");
			qstar_graph_free(&graph);
			return 1;
		}
		qstar_graph_free(&graph);
		return 0;
	}
	if (strcmp(cmd, "lsp") == 0) {
		int stdio_mode;

		stdio_mode = 0;
		while (arg < argc) {
			if (strcmp(argv[arg], "--stdio") == 0) {
				stdio_mode = 1;
				arg++;
			} else {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
		}
		if (!stdio_mode) {
			usage(stderr);
			qstar_graph_free(&graph);
			return 2;
		}
		qstar_graph_free(&graph);
		return qstar_lsp_stdio(stdin, stdout);
	}
	if (strcmp(cmd, "fmt") == 0) {
		const char *fmt_file;
		int fmt_check, fmt_stdout;

		fmt_file = file;
		fmt_check = 0;
		fmt_stdout = 0;
		while (arg < argc) {
			if (strcmp(argv[arg], "--check") == 0) {
				fmt_check = 1;
				arg++;
			} else if (strcmp(argv[arg], "--stdout") == 0) {
				fmt_stdout = 1;
				arg++;
			} else if (strncmp(argv[arg], "--", 2) == 0) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			} else {
				fmt_file = argv[arg++];
			}
		}
		rc = qstar_fmt_file(fmt_file, fmt_check, fmt_stdout, stdout);
		qstar_graph_free(&graph);
		return rc < 0 ? 1 : rc;
	}
	label = NULL;
	if (strcmp(cmd, "explain") == 0 || strcmp(cmd, "dry-run") == 0 ||
	    strcmp(cmd, "check") == 0 || strcmp(cmd, "query") == 0 ||
	    strcmp(cmd, "build") == 0 || strcmp(cmd, "test") == 0 ||
	    strcmp(cmd, "why-rebuild") == 0 || strcmp(cmd, "log") == 0 ||
	    strcmp(cmd, "action-log") == 0 || strcmp(cmd, "replay") == 0) {
		if (arg < argc)
			label = argv[arg++];
	} else if (strcmp(cmd, "lint") == 0) {
		while (arg < argc) {
			if (strcmp(argv[arg], "--format") == 0) {
				if (arg + 1 >= argc ||
				    (strcmp(argv[arg + 1], "text") != 0 &&
				    strcmp(argv[arg + 1], "json") != 0)) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				lint_format = argv[arg + 1];
				arg += 2;
			} else if (!label) {
				label = argv[arg++];
			} else {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
		}
	} else if (strcmp(cmd, "clean") == 0) {
		if (arg < argc && strcmp(argv[arg], "--target") == 0) {
			if (arg + 1 >= argc) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
			label = argv[arg + 1];
			arg += 2;
		}
	} else if (strcmp(cmd, "install") == 0) {
		while (arg < argc) {
			if (strcmp(argv[arg], "--prefix") == 0) {
				if (arg + 1 >= argc) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				install_options.prefix = argv[arg + 1];
				arg += 2;
			} else if (strcmp(argv[arg], "--dry-run") == 0) {
				install_options.dry_run = 1;
				arg++;
			} else if (!label) {
				label = argv[arg++];
			} else {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
		}
	} else if (strcmp(cmd, "stage") == 0) {
		while (arg < argc) {
			if (strcmp(argv[arg], "--root") == 0) {
				if (arg + 1 >= argc) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				stage_options.root = argv[arg + 1];
				arg += 2;
			} else if (strcmp(argv[arg], "--dry-run") == 0) {
				stage_options.dry_run = 1;
				arg++;
			} else if (!label) {
				label = argv[arg++];
			} else {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
		}
	} else if (strcmp(cmd, "list-targets") == 0) {
		while (arg < argc) {
			if (strcmp(argv[arg], "--format") == 0) {
				if (arg + 1 >= argc ||
				    (strcmp(argv[arg + 1], "text") != 0 &&
				    strcmp(argv[arg + 1], "json") != 0)) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				list_format = argv[arg + 1];
				arg += 2;
			} else {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
		}
	} else if (strcmp(cmd, "--dump-graph") != 0 &&
	    strcmp(cmd, "doctor") != 0 && strcmp(cmd, "last-failure") != 0) {
		usage(stderr);
		qstar_graph_free(&graph);
		return 2;
	}
	if (strcmp(cmd, "build") == 0) {
		while (arg < argc) {
			if (strcmp(argv[arg], "--explain-cache") == 0) {
				build_options.explain_cache = 1;
				arg++;
			} else if (strcmp(argv[arg], "--schedule-trace") == 0) {
				build_options.schedule_trace = 1;
				arg++;
			} else if (strcmp(argv[arg], "--jobs") == 0) {
				if (arg + 1 >= argc) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				if (sscanf(argv[arg + 1], "%d", &build_options.jobs) != 1 ||
				    build_options.jobs < 1 || build_options.jobs > 256) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				arg += 2;
			} else {
				break;
			}
		}
	}
	if (strcmp(cmd, "query") == 0 && (!label || !*label)) {
		usage(stderr);
		qstar_graph_free(&graph);
		return 2;
	}
	if ((strcmp(cmd, "action-log") == 0 || strcmp(cmd, "replay") == 0) &&
	    (!label || !*label)) {
		usage(stderr);
		qstar_graph_free(&graph);
		return 2;
	}
	if (strcmp(cmd, "stage") == 0 && (!label || !*label)) {
		usage(stderr);
		qstar_graph_free(&graph);
		return 2;
	}
	if (arg != argc) {
		usage(stderr);
		qstar_graph_free(&graph);
		return 2;
	}
	rc = qstar_graph_set_profile_input(&graph, cli_profile, NULL, NULL, NULL);
	if (rc == 0)
		rc = qstar_graph_load_profile_files(&graph, file);
	if (rc == 0)
		rc = qstar_graph_set_profile_input(&graph, cli_profile, cli_target,
		    cli_toolchain, cli_stdlib);
	if (rc == 0)
		rc = qstar_graph_validate_profile(&graph);
	if (rc == 0)
		rc = qstar_lua_eval_file(&graph, file);
	if (rc == 0)
		rc = qstar_graph_validate_packages(&graph);
	if (rc == 0)
		rc = qstar_graph_validate_generated_outputs(&graph);
	if (rc == 0)
		rc = qstar_graph_validate_sources(&graph);
	if (rc == 0)
		rc = qstar_graph_validate_headers(&graph);
	if (rc == 0 && (strcmp(cmd, "check") == 0 || strcmp(cmd, "doctor") == 0 ||
	    strcmp(cmd, "build") == 0 || strcmp(cmd, "test") == 0 ||
	    strcmp(cmd, "install") == 0 || strcmp(cmd, "stage") == 0 ||
	    strcmp(cmd, "why-rebuild") == 0 ||
	    strcmp(cmd, "lint") == 0))
		rc = qstar_graph_validate_file_inputs(&graph);
	if (strcmp(cmd, "lint") == 0) {
		if (rc < 0 && graph.error[0])
			(void)qstar_graph_add_lint_from_error(&graph);
		rc = qstar_graph_lint(&graph, label, lint_format, stdout);
		qstar_graph_free(&graph);
		return rc < 0 ? 1 : 0;
	}
	if (rc == 0) {
		if (strcmp(cmd, "explain") == 0)
			rc = qstar_graph_explain_plan(&graph, label, stdout);
		else if (strcmp(cmd, "dry-run") == 0)
			rc = qstar_graph_dry_run(&graph, label, stdout);
		else if (strcmp(cmd, "check") == 0)
			rc = qstar_graph_check(&graph, label, stdout);
		else if (strcmp(cmd, "build") == 0)
			rc = qstar_graph_build_with_options(&graph, label, &build_options, stdout);
		else if (strcmp(cmd, "test") == 0)
			rc = qstar_graph_test(&graph, label, stdout);
		else if (strcmp(cmd, "install") == 0)
			rc = qstar_graph_install(&graph, label, &install_options, stdout);
		else if (strcmp(cmd, "stage") == 0)
			rc = qstar_graph_stage(&graph, label, &stage_options, stdout);
		else if (strcmp(cmd, "why-rebuild") == 0)
			rc = qstar_graph_why_rebuild(&graph, label, stdout);
		else if (strcmp(cmd, "clean") == 0)
			rc = qstar_graph_clean(&graph, label, stdout);
		else if (strcmp(cmd, "log") == 0)
			rc = qstar_graph_log(&graph, label, stdout);
		else if (strcmp(cmd, "last-failure") == 0)
			rc = qstar_graph_last_failure(&graph, stdout);
		else if (strcmp(cmd, "action-log") == 0)
			rc = qstar_graph_action_log(&graph, label, stdout);
		else if (strcmp(cmd, "replay") == 0)
			rc = qstar_graph_replay_action(&graph, label, stdout);
		else if (strcmp(cmd, "doctor") == 0)
			rc = qstar_graph_doctor(&graph, stdout);
		else if (strcmp(cmd, "list-targets") == 0)
			rc = strcmp(list_format, "json") == 0 ?
			    qstar_graph_list_targets_json(&graph, stdout) :
			    qstar_graph_list_targets(&graph, stdout);
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
