#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
usage(FILE *out)
{
	fputs("usage: qstar [options] list-targets\n", out);
	fputs("       qstar [options] commands [--format text|json]\n", out);
	fputs("       qstar [options] <project-command> [command-options]\n", out);
	fputs("       qstar --version\n", out);
	fputs("       qstar version\n", out);
	fputs("       qstar docs [--path|--ai-index|--show path]\n", out);
	fputs("       qstar [options] list-targets --format json\n", out);
	fputs("       qstar [options] query [label]\n", out);
	fputs("       qstar [options] doctor\n", out);
	fputs("       qstar [options] check [label]\n", out);
	fputs("       qstar [options] lint [label|//...] [--format text|json]\n", out);
	fputs("       qstar [options] fmt [--check] [qstar.lua|fragment.qst|module.qsm]\n", out);
	fputs("       qstar [options] explain [label]\n", out);
	fputs("       qstar [options] dry-run [label]\n", out);
	fputs("       qstar [options] emit-ninja [label]\n", out);
	fputs("       qstar [options] build [label]\n", out);
	fputs("       qstar [options] test [label|//...]\n", out);
	fputs("       qstar [options] stage <label> [--root path] [--dry-run]\n", out);
	fputs("       qstar [options] why-rebuild [label]\n", out);
	fputs("       qstar [options] clean [label]\n", out);
	fputs("       qstar [options] log [label]\n", out);
	fputs("       qstar [options] last-failure\n", out);
	fputs("       qstar [options] action-log <action-id>\n", out);
	fputs("       qstar [options] replay <action-id>\n", out);
	fputs("       qstar [options] daemon --socket path --start|--stop|--serve|--status|--query method  # beta\n", out);
	fputs("       qstar lsp --stdio\n", out);
	fputs("       qstar init app|lib|tool|empty|workspace [directory] [--name name] [--use-language list] [--dry-run]\n", out);
	fputs("       qstar [options] --dump-graph\n", out);
	fputs("options:\n", out);
	fputs("       --file qstar.lua\n", out);
	fputs("       -G stella|ninja|auto\n", out);
	fputs("       --generator stella|ninja|auto\n", out);
	fputs("       -B path\n", out);
	fputs("       --package-alias @name=/path\n", out);
	fputs("       --diagnostics text|json\n", out);
	fputs("       --diagnostic-format text|line  # compatibility alias\n", out);
	fputs("       --color auto|always|never  # warning/error color for text output\n", out);
	fputs("build options:\n", out);
	fputs("       --jobs N  # default: host CPU count\n", out);
	fputs("       --progress auto|plain|off  # default: CMake-style action progress\n", out);
	fputs("       --verbose  # keep progress and add argv/cache/action details\n", out);
	fputs("       --schedule-trace  # add scheduler internals such as schedule_action\n", out);
	fputs("       --use-daemon auto|never|always  # beta Stella daemon client\n", out);
	fputs("       --daemon-socket path  # beta Unix socket path; Windows named pipe deferred\n", out);
	fputs("       --quiet\n", out);
}

/** argv item이 help 요청인지 확인한다. */
static int
is_help_arg(const char *arg)
{
	return arg && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0);
}

/** graph 평가 없이 정적 help를 출력할 수 있는 built-in command인지 확인한다. */
static int
command_has_static_help(const char *cmd)
{
	static const char *const commands[] = {
		"build", "daemon", "docs", "init", "test", "stage", "dry-run",
		"emit-ninja", "lint", "fmt", "list-targets", "commands",
		"check", "last-failure", "replay", "clean",
		"query", "doctor", "explain", "why-rebuild", "log",
		"action-log", NULL
	};
	size_t i;

	if (!cmd || !*cmd)
		return 0;
	for (i = 0; commands[i]; i++) {
		if (strcmp(cmd, commands[i]) == 0)
			return 1;
	}
	return 0;
}

/** subcommand별 help를 graph 평가 없이 출력한다. */
static void
command_help(FILE *out, const char *cmd)
{
	if (!cmd || !*cmd) {
		usage(out);
		return;
	}
	if (strcmp(cmd, "build") == 0) {
		fputs("usage: qstar [options] build [label] [--jobs N] [--schedule-trace] [--explain-cache] [--verbose|--quiet] [--progress auto|plain|off] [--use-daemon auto|never|always]\n", out);
		fputs("Build a target, generated action, run target, or group target in the validated graph.\n", out);
		fputs("--jobs defaults to the host CPU count when omitted.\n", out);
		fputs("Default progress uses CMake-style action lines such as '[ 75%] Linking CXX executable app'.\n", out);
		fputs("--progress auto is terminal-aware; --progress plain is deterministic; --progress off hides progress lines.\n", out);
		fputs("--verbose keeps progress and adds argv/cache/action details.\n", out);
		fputs("--schedule-trace adds scheduler internals; default output hides Stage/Status/schedule_action/build_action details.\n", out);
		fputs("--color controls warning:/error: ANSI color in text output; JSON diagnostics stay uncolored.\n", out);
		fputs("--use-daemon is experimental; auto falls back to normal Stella, always fails on daemon errors.\n", out);
		fputs("--daemon-socket selects the experimental Unix socket path; Windows named pipe support is deferred.\n", out);
		return;
	}
	if (strcmp(cmd, "daemon") == 0) {
		fputs("usage: qstar [options] daemon --socket path --start\n", out);
		fputs("       qstar [options] daemon --socket path --stop\n", out);
		fputs("       qstar [options] daemon --socket path --serve\n", out);
		fputs("       qstar [options] daemon --socket path --status\n", out);
		fputs("       qstar [options] daemon --socket path --query method\n", out);
		fputs("Run the beta opt-in persistent Stella daemon lifecycle on Unix socket hosts.\n", out);
		fputs("Windows named pipe daemon support is deferred.\n", out);
		fputs("Read-only query methods: hello, workspace.info, targets.list, diagnostics.list, compile_commands.path, build.summary.\n", out);
		fputs("This is not a stable public surface yet; normal qstar build is unchanged.\n", out);
		return;
	}
	if (strcmp(cmd, "docs") == 0) {
		fputs("usage: qstar docs [--path|--ai-index|--show wiki-relative.md]\n", out);
		fputs("Print local QStar documentation entrypoints for users and AI agents.\n", out);
		fputs("--path prints the wiki root; --ai-index prints AI_INDEX.md; --show prints a document.\n", out);
		fputs("Language provider docs cover bundled standard providers such as zig and project-local providers.\n", out);
		fputs("Generic workflow docs cover qstar.transform, run_target.inputs, qstar.command, and explicit layout export.\n", out);
		return;
	}
	if (strcmp(cmd, "init") == 0) {
		fputs("usage: qstar init app|lib|tool|empty|workspace [directory] [--name name] [--use-language list] [--dry-run]\n", out);
		fputs("       qstar init --list-shapes\n", out);
		fputs("       qstar init --list-languages\n", out);
		fputs("Create a starter project from a generic project shape.\n", out);
		fputs("The default language is c. External language providers are vendored into qstar/languages/<id>.\n", out);
		fputs("--name overrides the project name inferred from the directory basename.\n", out);
		fputs("--use-language accepts a comma-separated list such as c,zig; the first language is primary.\n", out);
		fputs("--dry-run prints the creation plan without writing files.\n", out);
		return;
	}
	if (strcmp(cmd, "test") == 0) {
		fputs("usage: qstar [options] test [label|//...]\n", out);
		fputs("Build and run qstar.test targets, storing stdout/stderr logs under build/qstar.\n", out);
		return;
	}
	if (strcmp(cmd, "stage") == 0) {
		fputs("usage: qstar [options] stage <label> [--root path] [--dry-run]\n", out);
		fputs("Create a copy-only staged package tree and qstar-stage-manifest-v2.\n", out);
		return;
	}
	if (strcmp(cmd, "dry-run") == 0) {
		fputs("usage: qstar [options] dry-run [label]\n", out);
		fputs("Render the command plan without executing actions.\n", out);
		return;
	}
	if (strcmp(cmd, "emit-ninja") == 0) {
		fputs("usage: qstar [options] emit-ninja [label]\n", out);
		fputs("Emit build/qstar/ninja/build.ninja and policy-controlled compile_commands.json.\n", out);
		fputs("Lowers C/C++/ASM compile, generated, staticlib, sharedlib, executable/test, run_target, and group edges.\n", out);
		return;
	}
	if (strcmp(cmd, "lint") == 0) {
		fputs("usage: qstar [options] lint [label|//...] [--format text|json]\n", out);
		fputs("Evaluate QStar authoring diagnostics without running build actions.\n", out);
		return;
	}
	if (strcmp(cmd, "fmt") == 0) {
		fputs("usage: qstar [options] fmt [--check] [--stdout] [qstar.lua|fragment.qst|module.qsm]\n", out);
		fputs("Format qstar.* authoring blocks while preserving ordinary Lua helpers.\n", out);
		return;
	}
	if (strcmp(cmd, "list-targets") == 0) {
		fputs("usage: qstar [options] list-targets [--format text|json]\n", out);
		fputs("List targets, generated actions, stages, and target families.\n", out);
		return;
	}
	if (strcmp(cmd, "commands") == 0) {
		fputs("usage: qstar [options] commands [--format text|json]\n", out);
		fputs("List root qstar.command project commands made of generic qstar.step.* operations.\n", out);
		return;
	}
	if (strcmp(cmd, "check") == 0) {
		fputs("usage: qstar [options] check [label|//...]\n", out);
		fputs("Validate the graph and input files without executing actions.\n", out);
		return;
	}
	if (strcmp(cmd, "last-failure") == 0) {
		fputs("usage: qstar [options] last-failure\n", out);
		fputs("Print the last failure replay script from the active build directory.\n", out);
		return;
	}
	if (strcmp(cmd, "replay") == 0) {
		fputs("usage: qstar [options] replay <action-id>\n", out);
		fputs("Print the stored replay command for an action id.\n", out);
		return;
	}
	usage(out);
}

/** path가 읽을 수 있는 일반 파일인지 가볍게 확인한다. */
static int
readable_file(const char *path)
{
	FILE *f;

	f = fopen(path, "rb");
	if (!f)
		return 0;
	fclose(f);
	return 1;
}

/** doc_dir 아래 wiki/README.md가 있는지 확인한다. */
static int
docs_dir_has_wiki(const char *doc_dir)
{
	char path[QSTAR_PATH_MAX];

	if (snprintf(path, sizeof(path), "%s/wiki/README.md", doc_dir) >=
	    (int)sizeof(path))
		return 0;
	return readable_file(path);
}

/** qstar executable 위치에서 설치 doc_dir 후보를 계산한다. */
static int
docs_dir_from_argv0(const char *argv0, char *doc_dir, size_t doc_dir_len)
{
	char bin_dir[QSTAR_PATH_MAX], prefix[QSTAR_PATH_MAX];

	if (!argv0 || !strchr(argv0, '/'))
		return -1;
	if (qstar_dirname(argv0, bin_dir, sizeof(bin_dir)) < 0 ||
	    qstar_dirname(bin_dir, prefix, sizeof(prefix)) < 0)
		return -1;
	if (snprintf(doc_dir, doc_dir_len, "%s/share/doc/qstar", prefix) >=
	    (int)doc_dir_len)
		return -1;
	return 0;
}

/** qstar/build/bin/qstar 형태의 개발 binary에서 source-tree doc root를 계산한다. */
static int
source_docs_dir_from_argv0(const char *argv0, char *doc_dir, size_t doc_dir_len)
{
	char bin_dir[QSTAR_PATH_MAX], build_dir[QSTAR_PATH_MAX], source_dir[QSTAR_PATH_MAX];

	if (!argv0 || !strchr(argv0, '/'))
		return -1;
	if (qstar_dirname(argv0, bin_dir, sizeof(bin_dir)) < 0 ||
	    qstar_dirname(bin_dir, build_dir, sizeof(build_dir)) < 0 ||
	    qstar_dirname(build_dir, source_dir, sizeof(source_dir)) < 0)
		return -1;
	if (snprintf(doc_dir, doc_dir_len, "%s", source_dir) >= (int)doc_dir_len)
		return -1;
	return 0;
}

/** QStar docs root를 환경 변수, 설치 layout, source tree 순서로 찾는다. */
static void
resolve_docs_dir(const char *argv0, char *doc_dir, size_t doc_dir_len)
{
	const char *env_doc_dir;
	char candidate[QSTAR_PATH_MAX];

	env_doc_dir = getenv("QSTAR_DOC_DIR");
	if (env_doc_dir && *env_doc_dir) {
		snprintf(doc_dir, doc_dir_len, "%s", env_doc_dir);
		return;
	}
	if (docs_dir_from_argv0(argv0, candidate, sizeof(candidate)) == 0 &&
	    docs_dir_has_wiki(candidate)) {
		snprintf(doc_dir, doc_dir_len, "%s", candidate);
		return;
	}
	if (source_docs_dir_from_argv0(argv0, candidate, sizeof(candidate)) == 0 &&
	    docs_dir_has_wiki(candidate)) {
		snprintf(doc_dir, doc_dir_len, "%s", candidate);
		return;
	}
	if (readable_file("wiki/README.md")) {
		snprintf(doc_dir, doc_dir_len, ".");
		return;
	}
	snprintf(doc_dir, doc_dir_len, "<prefix>/share/doc/qstar");
}

/** QStar 문서 위치와 문서 내용을 docs CLI mode에 맞게 출력한다. */
static int
print_docs(FILE *out, int path_only, int ai_index_only, const char *show_path,
    const char *argv0)
{
	char doc_dir[QSTAR_PATH_MAX], show_file[QSTAR_PATH_MAX];
	char buf[4096];
	const char *rel;
	FILE *f;

	resolve_docs_dir(argv0, doc_dir, sizeof(doc_dir));
	if (path_only) {
		fprintf(out, "%s/wiki\n", doc_dir);
		return 0;
	}
	if (ai_index_only) {
		fprintf(out, "%s/wiki/AI_INDEX.md\n", doc_dir);
		return 0;
	}
	if (show_path) {
		rel = show_path;
		if (strncmp(rel, "wiki/", 5) == 0)
			rel += 5;
		if (!qstar_path_is_package_relative(rel)) {
			fprintf(stderr,
			    "qstar: docs --show path must be wiki-relative\n");
			return -1;
		}
		if (snprintf(show_file, sizeof(show_file), "%s/wiki/%s", doc_dir, rel) >=
		    (int)sizeof(show_file)) {
			fprintf(stderr, "qstar: docs --show path is too long\n");
			return -1;
		}
		f = fopen(show_file, "rb");
		if (!f) {
			fprintf(stderr,
			    "qstar: docs file '%s' not found; set QSTAR_DOC_DIR if qstar is installed elsewhere\n",
			    show_path);
			return -1;
		}
		while (fgets(buf, sizeof(buf), f))
			fputs(buf, out);
		fclose(f);
		return 0;
	}
	fputs("qstar docs v1\n", out);
	fprintf(out, "installed_docs %s\n", doc_dir);
	fprintf(out, "wiki %s/wiki/README.md\n", doc_dir);
	fprintf(out, "ai_index %s/wiki/AI_INDEX.md\n", doc_dir);
	fprintf(out, "man qstar\n");
	fprintf(out, "man qstar-lua\n");
	fprintf(out, "show qstar docs --show reference/qstar-lua.md\n");
	fprintf(out, "source_tree wiki/README.md\n");
	fprintf(out, "source_tree wiki/AI_INDEX.md\n");
	return 0;
}

/** QStar runtime version을 CLI와 authoring 상수의 단일 source로 출력한다. */
static void
print_version(FILE *out)
{
	fprintf(out, "qstar %s\n", QSTAR_VERSION);
}

/** CLI progress mode 문자열을 내부 enum으로 변환한다. */
static int
parse_progress_mode(const char *s, int *mode)
{
	if (strcmp(s, "auto") == 0)
		*mode = QSTAR_PROGRESS_AUTO;
	else if (strcmp(s, "plain") == 0)
		*mode = QSTAR_PROGRESS_PLAIN;
	else if (strcmp(s, "off") == 0)
		*mode = QSTAR_PROGRESS_OFF;
	else
		return -1;
	return 0;
}

/** CLI color mode 문자열을 내부 enum으로 변환한다. */
static int
parse_color_mode(const char *s, int *mode)
{
	if (strcmp(s, "auto") == 0)
		*mode = QSTAR_COLOR_AUTO;
	else if (strcmp(s, "always") == 0)
		*mode = QSTAR_COLOR_ALWAYS;
	else if (strcmp(s, "never") == 0)
		*mode = QSTAR_COLOR_NEVER;
	else
		return -1;
	return 0;
}

/** stderr가 color 출력을 받을 수 있는지 결정한다. */
static int
stderr_color_enabled(int color_mode)
{
	if (color_mode == QSTAR_COLOR_ALWAYS)
		return 1;
	if (color_mode == QSTAR_COLOR_NEVER)
		return 0;
	return isatty(fileno(stderr));
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

/** Stella executor가 아직 필요한 command인지 확인한다. */
static int
command_requires_stella_generator(const char *cmd)
{
	(void)cmd;
	return 0;
}

/** QStar diagnostic을 text 또는 machine-readable skeleton으로 출력한다. */
static void
print_error(const struct qstar_graph *graph, const char *label, const char *format,
    int color_mode)
{
	const char *message;
	int use_color;

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
	use_color = stderr_color_enabled(color_mode);
	fprintf(stderr, "%s%s%s\n", use_color ? "\033[1;31m" : "", message,
	    use_color ? "\033[0m" : "");
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
	const char *cli_build_context, *cli_target, *cli_platform, *cli_toolchain, *cli_stdlib;
	const char *cli_generator, *cli_build_dir, *daemon_socket;
	struct qstar_build_options build_options;
	struct qstar_stage_options stage_options;
	char init_error[512];
	char plan_cache_reason[128], plan_cache_store_reason[128];
	int arg, rc, cli_overrides_applied, plan_cache_loaded, plan_cache_checked;
	int project_command_requested, project_command_argc;
	char **project_command_argv;
	int daemon_mode, daemon_status;

	qstar_graph_init(&graph);
	memset(&build_options, 0, sizeof(build_options));
	memset(&stage_options, 0, sizeof(stage_options));
	plan_cache_reason[0] = '\0';
	plan_cache_store_reason[0] = '\0';
	file = "qstar.lua";
	diagnostic_format = "text";
	lint_format = "text";
	list_format = "text";
	cli_build_context = NULL;
	cli_target = NULL;
	cli_platform = NULL;
	cli_toolchain = NULL;
	cli_stdlib = NULL;
	cli_generator = NULL;
	cli_build_dir = NULL;
	daemon_socket = NULL;
	daemon_mode = QSTAR_DAEMON_NEVER;
	project_command_requested = 0;
	project_command_argc = 0;
	project_command_argv = NULL;
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		print_version(stdout);
		qstar_graph_free(&graph);
		return 0;
	}
	if (argc == 2 && is_help_arg(argv[1])) {
		usage(stdout);
		qstar_graph_free(&graph);
		return 0;
	}
	arg = 1;
	build_options.progress_mode = QSTAR_PROGRESS_AUTO;
	build_options.color_mode = QSTAR_COLOR_AUTO;
	while (arg < argc &&
	    (strncmp(argv[arg], "--", 2) == 0 || strcmp(argv[arg], "-G") == 0 ||
	    strcmp(argv[arg], "-B") == 0 || is_help_arg(argv[arg])) &&
	    strcmp(argv[arg], "--dump-graph") != 0) {
		if (is_help_arg(argv[arg])) {
			usage(stdout);
			qstar_graph_free(&graph);
			return 0;
		} else if (strcmp(argv[arg], "--file") == 0) {
			if (arg + 1 >= argc) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
			file = argv[arg + 1];
			arg += 2;
		} else if (strcmp(argv[arg], "-G") == 0 ||
		    strcmp(argv[arg], "--generator") == 0) {
			if (arg + 1 >= argc) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
			cli_generator = argv[arg + 1];
			arg += 2;
		} else if (strcmp(argv[arg], "-B") == 0) {
			if (arg + 1 >= argc) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
			cli_build_dir = argv[arg + 1];
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
		} else if (strcmp(argv[arg], "--color") == 0) {
			if (arg + 1 >= argc ||
			    parse_color_mode(argv[arg + 1], &build_options.color_mode) < 0) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
			arg += 2;
		} else if (strcmp(argv[arg], "--progress") == 0) {
			if (arg + 1 >= argc ||
			    parse_progress_mode(argv[arg + 1],
			    &build_options.progress_mode) < 0) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
			arg += 2;
		} else if (strcmp(argv[arg], "--verbose") == 0) {
			build_options.verbose = 1;
			arg++;
		} else if (strcmp(argv[arg], "--quiet") == 0) {
			build_options.quiet = 1;
			arg++;
		} else if (strcmp(argv[arg], "--qstar-internal-target") == 0 ||
		    strcmp(argv[arg], "--qstar-internal-platform") == 0 ||
		    strcmp(argv[arg], "--qstar-internal-toolchain") == 0 ||
		    strcmp(argv[arg], "--qstar-internal-stdlib") == 0) {
			if (arg + 1 >= argc) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
			if (strcmp(argv[arg], "--qstar-internal-target") == 0)
				cli_target = argv[arg + 1];
			else if (strcmp(argv[arg], "--qstar-internal-platform") == 0)
				cli_platform = argv[arg + 1];
			else if (strcmp(argv[arg], "--qstar-internal-toolchain") == 0)
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
		if (!readable_file(file)) {
			usage(stderr);
			qstar_graph_free(&graph);
			return 2;
		}
		cmd = "";
		project_command_requested = 1;
		project_command_argc = 0;
		project_command_argv = argv + argc;
	} else {
		cmd = argv[arg++];
	}
	if (strcmp(cmd, "help") == 0) {
		if (arg < argc)
			command_help(stdout, argv[arg++]);
		else
			usage(stdout);
		if (arg != argc) {
			usage(stderr);
			qstar_graph_free(&graph);
			return 2;
		}
		qstar_graph_free(&graph);
		return 0;
	}
	if (arg < argc && is_help_arg(argv[arg]) && command_has_static_help(cmd)) {
		command_help(stdout, cmd);
		qstar_graph_free(&graph);
		return arg + 1 == argc ? 0 : 2;
	}
	build_options.jobs = 0;
	if (strcmp(cmd, "version") == 0) {
		if (arg != argc) {
			usage(stderr);
			qstar_graph_free(&graph);
			return 2;
		}
		print_version(stdout);
		qstar_graph_free(&graph);
		return 0;
	}
	if (strcmp(cmd, "docs") == 0) {
		const char *show_path;
		int path_only, ai_index_only, mode_count;

		path_only = 0;
		ai_index_only = 0;
		show_path = NULL;
		while (arg < argc) {
			if (strcmp(argv[arg], "--path") == 0) {
				path_only = 1;
				arg++;
			} else if (strcmp(argv[arg], "--ai-index") == 0) {
				ai_index_only = 1;
				arg++;
			} else if (strcmp(argv[arg], "--show") == 0) {
				if (arg + 1 >= argc) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				show_path = argv[arg + 1];
				arg += 2;
			} else {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
		}
		mode_count = path_only + ai_index_only + (show_path ? 1 : 0);
		if (mode_count > 1) {
			usage(stderr);
			qstar_graph_free(&graph);
			return 2;
		}
		if (print_docs(stdout, path_only, ai_index_only, show_path, argv[0]) < 0) {
			qstar_graph_free(&graph);
			return 1;
		}
		qstar_graph_free(&graph);
		return 0;
	}
	if (strcmp(cmd, "init") == 0) {
		struct qstar_init_options init_options;
		const char *positionals[2];
		size_t positional_count;
		int list_shapes, list_languages;

		memset(&init_options, 0, sizeof(init_options));
		init_options.directory = ".";
		init_options.use_language = "c";
		positionals[0] = NULL;
		positionals[1] = NULL;
		positional_count = 0;
		list_shapes = 0;
		list_languages = 0;
		while (arg < argc) {
			if (strcmp(argv[arg], "--dry-run") == 0) {
				init_options.dry_run = 1;
				arg++;
			} else if (strcmp(argv[arg], "--list-shapes") == 0) {
				list_shapes = 1;
				arg++;
			} else if (strcmp(argv[arg], "--list-languages") == 0) {
				list_languages = 1;
				arg++;
			} else if (strcmp(argv[arg], "--name") == 0) {
				if (arg + 1 >= argc) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				init_options.name = argv[arg + 1];
				arg += 2;
			} else if (strncmp(argv[arg], "--name=", 7) == 0) {
				init_options.name = argv[arg] + 7;
				arg++;
			} else if (strcmp(argv[arg], "--use-language") == 0) {
				if (arg + 1 >= argc) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				init_options.use_language = argv[arg + 1];
				arg += 2;
			} else if (strncmp(argv[arg], "--use-language=", 15) == 0) {
				init_options.use_language = argv[arg] + 15;
				arg++;
			} else if (strncmp(argv[arg], "--", 2) == 0) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			} else {
				if (positional_count >= 2) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				positionals[positional_count++] = argv[arg++];
			}
		}
		if (list_shapes || list_languages) {
			if (positional_count != 0 || init_options.name ||
			    strcmp(init_options.use_language, "c") != 0 ||
			    init_options.dry_run || (list_shapes && list_languages)) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
			if (list_shapes) {
				qstar_init_print_shapes(stdout);
			} else {
				init_error[0] = '\0';
				if (qstar_init_print_languages(stdout, init_error,
				    sizeof(init_error)) < 0) {
					fprintf(stderr, "%s\n", init_error[0] ? init_error :
					    "qstar: init failed");
					qstar_graph_free(&graph);
					return 1;
				}
			}
			qstar_graph_free(&graph);
			return 0;
		}
		if (positional_count == 0) {
			usage(stderr);
			qstar_graph_free(&graph);
			return 2;
		}
		init_options.shape = positionals[0];
		if (positional_count > 1)
			init_options.directory = positionals[1];
		init_error[0] = '\0';
		rc = qstar_init_project(&init_options, stdout, init_error, sizeof(init_error));
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
	if (strcmp(cmd, "daemon") == 0) {
		rc = qstar_daemon_command(argc - arg, argv + arg, file, cli_build_dir,
		    cli_build_context, cli_target, cli_platform, cli_toolchain, cli_stdlib,
		    stdout);
		qstar_graph_free(&graph);
		return rc;
	}
	label = NULL;
	cli_overrides_applied = 0;
	plan_cache_loaded = 0;
	plan_cache_checked = 0;
	if (strcmp(cmd, "explain") == 0 || strcmp(cmd, "dry-run") == 0 ||
	    strcmp(cmd, "emit-ninja") == 0 ||
	    strcmp(cmd, "check") == 0 || strcmp(cmd, "query") == 0 ||
	    strcmp(cmd, "test") == 0 ||
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
		while (arg < argc) {
			if (strcmp(argv[arg], "--target") == 0) {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
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
	} else if (strcmp(cmd, "commands") == 0) {
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
	    strcmp(cmd, "doctor") != 0 && strcmp(cmd, "last-failure") != 0 &&
	    strcmp(cmd, "build") != 0) {
		project_command_requested = 1;
		project_command_argc = argc - arg;
		project_command_argv = argv + arg;
		arg = argc;
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
			} else if (strcmp(argv[arg], "--progress") == 0) {
				if (arg + 1 >= argc ||
				    parse_progress_mode(argv[arg + 1],
				    &build_options.progress_mode) < 0) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				arg += 2;
			} else if (strcmp(argv[arg], "--color") == 0) {
				if (arg + 1 >= argc ||
				    parse_color_mode(argv[arg + 1],
				    &build_options.color_mode) < 0) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				arg += 2;
			} else if (strcmp(argv[arg], "--verbose") == 0) {
				build_options.verbose = 1;
				arg++;
			} else if (strcmp(argv[arg], "--quiet") == 0) {
				build_options.quiet = 1;
				arg++;
			} else if (strncmp(argv[arg], "--use-daemon=", 13) == 0) {
				if (qstar_daemon_parse_mode(argv[arg] + 13, &daemon_mode) < 0) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				arg++;
			} else if (strcmp(argv[arg], "--use-daemon") == 0) {
				if (arg + 1 >= argc ||
				    qstar_daemon_parse_mode(argv[arg + 1], &daemon_mode) < 0) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				arg += 2;
			} else if (strcmp(argv[arg], "--daemon-socket") == 0) {
				if (arg + 1 >= argc) {
					usage(stderr);
					qstar_graph_free(&graph);
					return 2;
				}
				daemon_socket = argv[arg + 1];
				arg += 2;
			} else if (!label) {
				label = argv[arg++];
			} else {
				usage(stderr);
				qstar_graph_free(&graph);
				return 2;
			}
		}
	}
	if (strcmp(cmd, "query") == 0 && (!label || !*label)) {
		usage(stderr);
		qstar_graph_free(&graph);
		return 2;
	}
	if (strcmp(cmd, "check") == 0 && label && strcmp(label, "//...") == 0)
		label = NULL;
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
	rc = qstar_graph_set_build_context_input(&graph, cli_build_context, NULL, NULL, NULL);
	if (rc == 0)
		rc = qstar_graph_set_platform_context(&graph, cli_platform);
	if (rc == 0 && (strcmp(cmd, "build") == 0 || project_command_requested)) {
		rc = qstar_graph_set_cli_overrides(&graph, cli_generator, cli_build_dir);
		cli_overrides_applied = rc == 0;
	}
	if (rc == 0 && strcmp(cmd, "build") == 0 &&
	    daemon_mode != QSTAR_DAEMON_NEVER) {
		char daemon_error[512];
		int client_rc;

		if (strcmp(qstar_graph_generator(&graph), "ninja") == 0) {
			if (daemon_mode == QSTAR_DAEMON_ALWAYS)
				rc = qstar_set_error(&graph,
				    "qstar: --use-daemon=always is only supported with the Stella generator");
		} else {
			daemon_status = 1;
			client_rc = qstar_daemon_build_client(daemon_socket, daemon_mode,
			    file, label, cli_build_dir, cli_build_context, cli_target,
			    cli_platform, cli_toolchain, cli_stdlib, &build_options, stdout,
			    &daemon_status, daemon_error, sizeof(daemon_error));
			if (client_rc == 0) {
				qstar_graph_free(&graph);
				return daemon_status == 0 ? 0 : 1;
			}
			if (daemon_mode == QSTAR_DAEMON_ALWAYS) {
				rc = qstar_set_error(&graph, "qstar: daemon build failed: %s",
				    daemon_error[0] ? daemon_error : "unknown");
			} else if (build_options.schedule_trace || build_options.verbose) {
				fprintf(stdout,
				    "daemon status=unavailable reason=%s fallback=stella\n",
				    daemon_error[0] ? daemon_error : "unknown");
			}
		}
	}
	if (rc == 0 && strcmp(cmd, "build") == 0 &&
	    strcmp(qstar_graph_generator(&graph), "stella") == 0) {
		plan_cache_checked = 1;
		plan_cache_loaded = qstar_stella_plan_cache_try_load(&graph, file, cmd,
		    label, cli_build_context, cli_target, cli_platform, cli_toolchain, cli_stdlib,
		    plan_cache_reason, sizeof(plan_cache_reason));
	}
	if (rc == 0 && !plan_cache_loaded)
		rc = qstar_lua_eval_file(&graph, file);
	if (rc == 0 && !cli_overrides_applied)
		rc = qstar_graph_set_cli_overrides(&graph, cli_generator, cli_build_dir);
	if (rc == 0 && !plan_cache_loaded)
		rc = qstar_graph_set_build_context_input(&graph, cli_build_context, cli_target,
		    cli_toolchain, cli_stdlib);
	if (rc == 0 && !plan_cache_loaded)
		rc = qstar_graph_set_platform_context(&graph, cli_platform);
	if (rc == 0 && !plan_cache_loaded)
		rc = qstar_graph_validate_build_context(&graph);
	if (rc == 0 && !plan_cache_loaded)
		rc = qstar_graph_validate_toolsets(&graph);
	if (rc == 0 && !plan_cache_loaded)
		rc = qstar_graph_validate_packages(&graph);
	if (rc == 0 && !plan_cache_loaded)
		rc = qstar_graph_validate_generated_outputs(&graph);
	if (rc == 0 && !plan_cache_loaded)
		rc = qstar_graph_validate_sources(&graph);
	if (rc == 0 && !plan_cache_loaded)
		rc = qstar_graph_validate_headers(&graph);
	if (rc == 0 && !plan_cache_loaded)
		rc = qstar_graph_validate_project_commands(&graph);
	if (rc == 0 && !plan_cache_loaded &&
	    (strcmp(cmd, "check") == 0 || strcmp(cmd, "doctor") == 0 ||
	    strcmp(cmd, "build") == 0 || strcmp(cmd, "test") == 0 ||
	    strcmp(cmd, "emit-ninja") == 0 ||
	    strcmp(cmd, "stage") == 0 ||
	    strcmp(cmd, "why-rebuild") == 0 ||
	    strcmp(cmd, "lint") == 0 || project_command_requested))
		rc = qstar_graph_validate_file_inputs(&graph);
	if (rc == 0 && strcmp(cmd, "build") == 0 &&
	    strcmp(qstar_graph_generator(&graph), "stella") == 0 &&
	    !plan_cache_loaded) {
		int stored;

		stored = qstar_stella_plan_cache_store(&graph, file, cmd, label,
		    cli_build_context, cli_target, cli_platform, cli_toolchain, cli_stdlib,
		    plan_cache_store_reason, sizeof(plan_cache_store_reason));
		if (stored < 0)
			rc = qstar_set_error(&graph, "qstar: could not write Stella plan cache: %s",
			    plan_cache_store_reason[0] ? plan_cache_store_reason : "unknown");
	}
	if (rc == 0 && strcmp(cmd, "build") == 0 && build_options.schedule_trace &&
	    plan_cache_checked)
		fprintf(stdout, "plan_cache status=%s reason=%s\n",
		    plan_cache_loaded ? "hit" : "miss",
		    plan_cache_reason[0] ? plan_cache_reason : "unknown");
	if (strcmp(cmd, "lint") == 0) {
		if (rc < 0 && graph.error[0])
			(void)qstar_graph_add_lint_from_error(&graph);
		rc = qstar_graph_lint_with_color(&graph, label, lint_format,
		    build_options.color_mode, stdout);
		qstar_graph_free(&graph);
		return rc < 0 ? 1 : 0;
	}
	if (rc == 0 && command_requires_stella_generator(cmd) &&
	    strcmp(qstar_graph_generator(&graph), "stella") != 0)
		rc = qstar_set_error(&graph,
		    "qstar: generator '%s' is recognized but action execution is not implemented yet; use -G stella",
		    qstar_graph_generator(&graph));
	if (rc == 0) {
		if (strcmp(cmd, "explain") == 0)
			rc = qstar_graph_explain_plan(&graph, label, stdout);
		else if (strcmp(cmd, "dry-run") == 0)
			rc = qstar_graph_dry_run(&graph, label, stdout);
		else if (strcmp(cmd, "check") == 0)
			rc = qstar_graph_check(&graph, label, stdout);
		else if (strcmp(cmd, "emit-ninja") == 0)
			rc = qstar_graph_emit_ninja(&graph, label, stdout);
		else if (strcmp(cmd, "build") == 0)
			rc = strcmp(qstar_graph_generator(&graph), "ninja") == 0 ?
			    qstar_graph_build_ninja(&graph, label, &build_options, stdout) :
			    qstar_graph_build_with_options(&graph, label, &build_options, stdout);
		else if (strcmp(cmd, "test") == 0)
			rc = strcmp(qstar_graph_generator(&graph), "ninja") == 0 ?
			    qstar_graph_test_ninja(&graph, label, stdout) :
			    qstar_graph_test(&graph, label, stdout);
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
		else if (strcmp(cmd, "commands") == 0)
			rc = strcmp(list_format, "json") == 0 ?
			    qstar_graph_list_project_commands_json(&graph, stdout) :
			    qstar_graph_list_project_commands(&graph, stdout);
		else if (strcmp(cmd, "query") == 0)
			rc = qstar_graph_query(&graph, label, stdout);
		else if (project_command_requested)
			rc = qstar_graph_run_project_command(&graph, cmd,
			    project_command_argc, project_command_argv,
			    &build_options, stdout);
		else
			rc = qstar_graph_dump(&graph, label, stdout);
	}
	if (rc < 0) {
		print_error(&graph, label, diagnostic_format, build_options.color_mode);
		qstar_graph_free(&graph);
		return 1;
	}
	qstar_graph_free(&graph);
	return 0;
}
