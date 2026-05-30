#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define QSTAR_EXEC_MAX_ARGV 256

struct qstar_build_ctx {
	FILE *out;
	const char *root_label;
};

/** 디렉터리를 parent까지 포함해 만든다. */
static int
mkdir_p(const char *path)
{
	char tmp[QSTAR_PATH_MAX];
	char *p;

	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
		return -1;
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

/** 파일 path의 parent directory를 package root 기준으로 만든다. */
static int
mkdir_parent_under_root(const struct qstar_graph *graph, const char *rel)
{
	char full[QSTAR_PATH_MAX];
	char *slash;

	if (qstar_path_join(graph->package_root ? graph->package_root : ".", rel, full,
	    sizeof(full)) < 0)
		return -1;
	slash = strrchr(full, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	return mkdir_p(full);
}

/** action id를 log filename으로 쓸 수 있게 정규화한다. */
static void
action_log_name(const char *id, char *dst, size_t dstlen)
{
	qstar_mangle_label(id, dst, dstlen);
}

/** argv 배열을 action log에 deterministic하게 기록한다. */
static void
write_action_log(const char *path, char *const argv[], int exit_code)
{
	FILE *f;
	size_t i;

	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "exit=%d\nargv=", exit_code);
	for (i = 0; argv[i]; i++)
		fprintf(f, "%s%s", i ? " " : "", argv[i]);
	fputc('\n', f);
	fclose(f);
}

/** 실패한 action을 직접 재현할 수 있는 argv 파일을 갱신한다. */
static void
write_failure_replay(const struct qstar_graph *graph, char *const argv[])
{
	char path[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;

	if (qstar_path_join(graph->package_root ? graph->package_root : ".",
	    ".qstar/logs/last-failure.replay", path, sizeof(path)) < 0)
		return;
	f = fopen(path, "w");
	if (!f)
		return;
	for (i = 0; argv[i]; i++)
		fprintf(f, "%s%s", i ? " " : "", argv[i]);
	fputc('\n', f);
	fclose(f);
}

/** stdout/stderr를 log 파일에 연결해 package root 안에서 argv를 실행한다. */
static int
run_action(struct qstar_graph *graph, FILE *out, const char *id, char *const argv[])
{
	char logdir[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX], stdout_path[QSTAR_PATH_MAX];
	char stderr_path[QSTAR_PATH_MAX], action_log[QSTAR_PATH_MAX];
	pid_t pid;
	int status, fdout, fderr, exit_code;

	if (qstar_path_join(graph->package_root ? graph->package_root : ".",
	    ".qstar/logs", logdir, sizeof(logdir)) < 0 ||
	    mkdir_p(logdir) < 0)
		return qstar_set_error(graph, "qstar: could not create action log dir");
	action_log_name(id, name, sizeof(name));
	snprintf(stdout_path, sizeof(stdout_path), "%s/%s.stdout", logdir, name);
	snprintf(stderr_path, sizeof(stderr_path), "%s/%s.stderr", logdir, name);
	snprintf(action_log, sizeof(action_log), "%s/%s.log", logdir, name);
	fprintf(out, "build_action id=%s stdout=.qstar/logs/%s.stdout stderr=.qstar/logs/%s.stderr\n",
	    id, name, name);
	pid = fork();
	if (pid < 0)
		return qstar_set_error(graph, "qstar: fork failed");
	if (pid == 0) {
		if (chdir(graph->package_root ? graph->package_root : ".") < 0)
			_exit(127);
		fdout = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		fderr = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fdout < 0 || fderr < 0 || dup2(fdout, 1) < 0 || dup2(fderr, 2) < 0)
			_exit(127);
		close(fdout);
		close(fderr);
		execvp(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return qstar_set_error(graph, "qstar: waitpid failed");
	exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
	write_action_log(action_log, argv, exit_code);
	if (exit_code != 0) {
		write_failure_replay(graph, argv);
		return qstar_set_error(graph,
		    "qstar: action '%s' failed with status %d; replay=.qstar/logs/last-failure.replay",
		    id, exit_code);
	}
	return 0;
}

/** graph에서 canonical label target을 찾는다. */
static const struct qstar_target *
find_target(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->len; i++) {
		if (strcmp(graph->targets[i].label, label) == 0)
			return &graph->targets[i];
	}
	return NULL;
}

/** target이 소비하는 generated action을 package-local tool policy로 실행한다. */
static int
run_generated_actions(struct qstar_graph *graph, FILE *out, const struct qstar_target *target)
{
	const struct qstar_genrule *genrule;
	char tool_path[QSTAR_PATH_MAX], id[QSTAR_PATH_MAX];
	char *argv[QSTAR_EXEC_MAX_ARGV];
	size_t i, j, argc;

	for (i = 0; i < graph->genrule_len; i++) {
		genrule = &graph->genrules[i];
		for (j = 0; j < genrule->outputs.len; j++) {
			size_t s;
			for (s = 0; s < target->sources.len; s++) {
				if (strcmp(genrule->outputs.items[j], target->sources.items[s]) != 0)
					continue;
				if (!qstar_path_is_package_relative(genrule->tool))
					return qstar_set_error(graph,
					    "qstar: generated action tool '%s' must be package-relative",
					    genrule->tool);
				if (qstar_path_join(graph->package_root ? graph->package_root : ".",
				    genrule->tool, tool_path, sizeof(tool_path)) < 0)
					return qstar_set_error(graph, "qstar: generated tool path too long");
				if (genrule->args.len + 2 > QSTAR_EXEC_MAX_ARGV)
					return qstar_set_error(graph, "qstar: generated action argv too long");
				for (argc = 0; argc < genrule->outputs.len; argc++) {
					if (mkdir_parent_under_root(graph, genrule->outputs.items[argc]) < 0)
						return qstar_set_error(graph,
						    "qstar: could not create generated output directory");
				}
				snprintf(id, sizeof(id), "%s:generate:0", genrule->label);
				argv[0] = tool_path;
				for (argc = 0; argc < genrule->args.len; argc++)
					argv[argc + 1] = genrule->args.items[argc];
				argv[genrule->args.len + 1] = NULL;
				if (run_action(graph, out, id, argv) < 0)
					return -1;
				break;
			}
		}
	}
	return 0;
}

/** C source 하나를 object로 compile한다. */
static int
run_compile(struct qstar_graph *graph, FILE *out, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, size_t index)
{
	struct qstar_source_info source;
	char object[QSTAR_PATH_MAX], id[QSTAR_PATH_MAX], target_arg[QSTAR_PATH_MAX];
	char *argv[QSTAR_EXEC_MAX_ARGV];
	size_t argc, i;

	qstar_source_classify(target->sources.items[index], &source);
	if (strcmp(source.language, "c") != 0)
		return qstar_set_error(graph,
		    "qstar: local executor v1 only supports C source compile, saw '%s'",
		    target->sources.items[index]);
	if (qstar_object_output_path(target, index, object, sizeof(object)) < 0 ||
	    mkdir_parent_under_root(graph, object) < 0)
		return qstar_set_error(graph, "qstar: could not create object output directory");
	if (target->include_dirs.len * 2 + target->system_include_dirs.len * 2 + 8 >
	    QSTAR_EXEC_MAX_ARGV)
		return qstar_set_error(graph, "qstar: compile argv too long");
	snprintf(id, sizeof(id), "%s:compile:%zu", target->label, index);
	snprintf(target_arg, sizeof(target_arg), "--target=%s", toolchain->target);
	argc = 0;
	argv[argc++] = (char *)toolchain->cc;
	if (strcmp(toolchain->name, "clang") == 0 && strcmp(toolchain->target, "host") != 0)
		argv[argc++] = target_arg;
	argv[argc++] = "-c";
	argv[argc++] = target->sources.items[index];
	argv[argc++] = "-o";
	argv[argc++] = object;
	for (i = 0; i < target->include_dirs.len; i++) {
		argv[argc++] = "-I";
		argv[argc++] = target->include_dirs.items[i];
	}
	for (i = 0; i < target->system_include_dirs.len; i++) {
		argv[argc++] = "-isystem";
		argv[argc++] = target->system_include_dirs.items[i];
	}
	argv[argc] = NULL;
	return run_action(graph, out, id, argv);
}

/** target의 direct dependency artifact를 final argv 뒤에 붙인다. */
static int
append_dep_artifacts(struct qstar_graph *graph, const struct qstar_target *target,
    char **argv, size_t *argc)
{
	const struct qstar_target *dep;
	char artifact[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < target->deps.len; i++) {
		if (target->deps.items[i][0] == '@')
			continue;
		dep = find_target(graph, target->deps.items[i]);
		if (!dep)
			continue;
		if (qstar_artifact_output_path(dep, artifact, sizeof(artifact)) < 0)
			return qstar_set_error(graph, "qstar: dependency artifact path too long");
		argv[(*argc)++] = qstar_strdup(artifact);
		if (!argv[*argc - 1])
			return qstar_set_error(graph, "qstar: out of memory");
	}
	return 0;
}

/** append_dep_artifacts에서 복사한 argv tail을 해제한다. */
static void
free_dep_artifacts(char **argv, size_t first, size_t argc)
{
	while (first < argc)
		free(argv[first++]);
}

/** target final archive/link action을 실행한다. */
static int
run_final(struct qstar_graph *graph, FILE *out, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain)
{
	char artifact[QSTAR_PATH_MAX], object[QSTAR_PATH_MAX], id[QSTAR_PATH_MAX];
	char *argv[QSTAR_EXEC_MAX_ARGV];
	size_t argc, dep_first, i;
	int rc;

	if (strcmp(target->kind, "exe") != 0 && strcmp(target->kind, "staticlib") != 0)
		return qstar_set_error(graph,
		    "qstar: local executor v1 only supports exe/staticlib targets");
	if (qstar_artifact_output_path(target, artifact, sizeof(artifact)) < 0 ||
	    mkdir_parent_under_root(graph, artifact) < 0)
		return qstar_set_error(graph, "qstar: could not create artifact output directory");
	argc = 0;
	if (strcmp(target->kind, "staticlib") == 0) {
		snprintf(id, sizeof(id), "%s:archive:0", target->label);
		argv[argc++] = (char *)toolchain->ar;
		argv[argc++] = "rcs";
		argv[argc++] = artifact;
	} else {
		snprintf(id, sizeof(id), "%s:link:0", target->label);
		argv[argc++] = (char *)toolchain->linker;
		argv[argc++] = "-o";
		argv[argc++] = artifact;
	}
	for (i = 0; i < target->sources.len; i++) {
		if (qstar_object_output_path(target, i, object, sizeof(object)) < 0)
			return qstar_set_error(graph, "qstar: object output path too long");
		argv[argc++] = qstar_strdup(object);
		if (!argv[argc - 1])
			return qstar_set_error(graph, "qstar: out of memory");
	}
	dep_first = argc;
	if (append_dep_artifacts(graph, target, argv, &argc) < 0) {
		free_dep_artifacts(argv, 3, dep_first);
		return -1;
	}
	argv[argc] = NULL;
	rc = run_action(graph, out, id, argv);
	free_dep_artifacts(argv, 3, argc);
	return rc;
}

/** closure target 하나를 local executor v1 policy로 실행한다. */
static int
build_target(struct qstar_graph *graph, const struct qstar_target *target, size_t order,
    void *user)
{
	struct qstar_build_ctx *ctx = user;
	struct qstar_resolved_toolchain toolchain;
	size_t i;

	fprintf(ctx->out, "build_target %s order=%zu kind=%s\n", target->label, order,
	    target->kind);
	if (qstar_resolve_toolchain(graph, target, &toolchain) < 0)
		return -1;
	fprintf(ctx->out, "resolved_toolchain owner=%s toolchain=%s target=%s cc=%s ar=%s linker=%s\n",
	    target->label, toolchain.name, toolchain.target, toolchain.cc, toolchain.ar,
	    toolchain.linker);
	if (run_generated_actions(graph, ctx->out, target) < 0)
		return -1;
	for (i = 0; i < target->sources.len; i++) {
		if (run_compile(graph, ctx->out, target, &toolchain, i) < 0)
			return -1;
	}
	return run_final(graph, ctx->out, target, &toolchain);
}

/** QStar local executor v1로 제한된 build action을 실행한다. */
int
qstar_graph_build(struct qstar_graph *graph, const char *label, FILE *out)
{
	struct qstar_build_ctx ctx;

	ctx.out = out;
	ctx.root_label = label && *label ? label : "<all>";
	fputs("qstar build v1\n", out);
	fprintf(out, "root %s\n", ctx.root_label);
	fprintf(out, "package-root %s\n", graph->package_root ? graph->package_root : ".");
	return qstar_graph_visit_closure(graph, label, build_target, &ctx) < 0 ? -1 :
	    (fputs("status ok\n", out), 0);
}
