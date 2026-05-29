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

struct qstar_graph {
	struct qstar_target *targets;
	size_t len;
	size_t cap;
	char error[512];
};

/** QStar graph 저장소를 빈 상태로 초기화한다. */
void qstar_graph_init(struct qstar_graph *graph);

/** QStar graph가 소유한 모든 동적 메모리를 해제한다. */
void qstar_graph_free(struct qstar_graph *graph);

/** QStar graph에 새 target을 추가하고 중복 label을 stable error로 막는다. */
struct qstar_target *qstar_graph_add_target(struct qstar_graph *graph, const char *label,
    const char *name, const char *kind, const char *fragment_dir);

/** QStar label을 현재 fragment 기준 canonical label로 정규화한다. */
int qstar_label_canonicalize(const char *label, const char *fragment_dir, char *dst, size_t dstlen);

/** QStar Graph IR를 deterministic explain text로 출력한다. */
int qstar_graph_dump(const struct qstar_graph *graph, const char *label, FILE *out);

/** QStar target closure와 non-executing command plan을 deterministic text로 출력한다. */
int qstar_graph_explain_plan(struct qstar_graph *graph, const char *label, FILE *out);

/** qstar.lua 파일을 sandboxed Lua runtime으로 평가해 Graph IR를 만든다. */
int qstar_lua_eval_file(struct qstar_graph *graph, const char *file);

#endif
