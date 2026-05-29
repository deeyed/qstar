#ifndef QSTAR_QSTAR_H
#define QSTAR_QSTAR_H

#include <stddef.h>
#include <stdio.h>

#ifndef QSTAR_PATH_MAX
#define QSTAR_PATH_MAX 4096
#endif

struct qstar_string_list {
	char **items;
	size_t len;
	size_t cap;
};

struct qstar_modules {
	int present;
	char *root;
	struct qstar_string_list include;
	struct qstar_string_list exclude;
};

struct qstar_target {
	char *label;
	char *name;
	char *kind;
	char *fragment_dir;
	struct qstar_modules modules;
	struct qstar_string_list sources;
	struct qstar_string_list public_headers;
	struct qstar_string_list private_headers;
	struct qstar_string_list include_dirs;
	struct qstar_string_list system_include_dirs;
	struct qstar_string_list deps;
	char *toolchain;
	char *stdlib_policy;
};

struct qstar_genrule {
	char *label;
	char *name;
	char *fragment_dir;
	char *tool;
	struct qstar_string_list inputs;
	struct qstar_string_list outputs;
	struct qstar_string_list args;
};

struct qstar_package_alias {
	char *alias;
	char *root;
};

struct qstar_profile_input {
	char *name;
	char *target;
	char *toolchain;
	char *stdlib_policy;
};

struct qstar_source_info {
	const char *path;
	const char *language;
	const char *tool_role;
};

struct qstar_graph {
	struct qstar_target *targets;
	size_t len;
	size_t cap;
	struct qstar_package_alias *packages;
	size_t package_len;
	size_t package_cap;
	struct qstar_genrule *genrules;
	size_t genrule_len;
	size_t genrule_cap;
	struct qstar_profile_input profile;
	char error[512];
};

/** QStar graph 저장소를 빈 상태로 초기화한다. */
void qstar_graph_init(struct qstar_graph *graph);

/** QStar graph가 소유한 모든 동적 메모리를 해제한다. */
void qstar_graph_free(struct qstar_graph *graph);

/** QStar graph에 새 target을 추가하고 중복 label을 stable error로 막는다. */
struct qstar_target *qstar_graph_add_target(struct qstar_graph *graph, const char *label,
    const char *name, const char *kind, const char *fragment_dir);

/** QStar graph에 generated action skeleton을 추가하고 중복 label을 막는다. */
struct qstar_genrule *qstar_graph_add_genrule(struct qstar_graph *graph, const char *label,
    const char *name, const char *fragment_dir);

/** QStar package alias를 추가하고 중복 alias를 stable error로 막는다. */
int qstar_graph_add_package_alias(struct qstar_graph *graph, const char *alias, const char *root);

/** QStar explain profile 입력을 graph에 기록한다. */
int qstar_graph_set_profile_input(struct qstar_graph *graph, const char *name,
    const char *target, const char *toolchain, const char *stdlib_policy);

/** QStar package alias map에서 alias를 찾는다. */
const struct qstar_package_alias *qstar_graph_find_package_alias(const struct qstar_graph *graph,
    const char *alias);

/** QStar header file graph policy를 검증한다. */
int qstar_graph_validate_headers(struct qstar_graph *graph);

/** QStar source path와 language classification을 검증한다. */
int qstar_graph_validate_sources(struct qstar_graph *graph);

/** QStar generated output edge skeleton을 검증한다. */
int qstar_graph_validate_generated_outputs(struct qstar_graph *graph);

/** generated output path를 생산하는 action skeleton을 찾는다. */
const struct qstar_genrule *qstar_graph_find_output_owner(const struct qstar_graph *graph,
    const char *path);

/** QStar source path를 language/tool role로 분류한다. */
int qstar_source_classify(const char *path, struct qstar_source_info *info);

/** QStar target의 header file plan skeleton을 출력한다. */
void qstar_target_dump_header_files(const struct qstar_target *target, FILE *out);

/** QStar target의 source discovery skeleton을 출력한다. */
void qstar_target_dump_source_discovery(const struct qstar_target *target, FILE *out);

/** QStar label을 현재 fragment 기준 canonical label로 정규화한다. */
int qstar_label_canonicalize(const char *label, const char *fragment_dir, char *dst, size_t dstlen);

/** QStar Graph IR를 deterministic explain text로 출력한다. */
int qstar_graph_dump(const struct qstar_graph *graph, const char *label, FILE *out);

/** QStar target closure와 non-executing command plan을 deterministic text로 출력한다. */
int qstar_graph_explain_plan(struct qstar_graph *graph, const char *label, FILE *out);

/** QStar target closure를 실제 실행 없이 dry-run command stream으로 출력한다. */
int qstar_graph_dry_run(struct qstar_graph *graph, const char *label, FILE *out);

/** qstar.lua 파일을 sandboxed Lua runtime으로 평가해 Graph IR를 만든다. */
int qstar_lua_eval_file(struct qstar_graph *graph, const char *file);

#endif
