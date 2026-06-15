#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int
valid_build_context_path(const char *path)
{
	return path && *path && (path[0] == '/' || qstar_path_is_package_relative(path));
}

/** build context boolean string이 QStar v3 policy 안에 있는지 검사한다. */
static int
valid_build_context_bool(const char *value)
{
	return !value || !*value || strcmp(value, "true") == 0 ||
	    strcmp(value, "false") == 0 || strcmp(value, "1") == 0 ||
	    strcmp(value, "0") == 0 || strcmp(value, "yes") == 0 ||
	    strcmp(value, "no") == 0 || strcmp(value, "on") == 0 ||
	    strcmp(value, "off") == 0;
}

/** target artifact filename이 package-local basename으로 안전한지 검사한다. */
static int
valid_artifact_name(const char *value)
{
	const unsigned char *p;

	if (!value || !*value)
		return 0;
	for (p = (const unsigned char *)value; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' || *p == '+'))
			return 0;
	}
	return 1;
}

/** build context artifact_names entry의 target key를 제한한다. */
static int
valid_artifact_key(const char *key)
{
	const unsigned char *p;

	if (!key || !*key)
		return 0;
	for (p = (const unsigned char *)key; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' ||
		    *p == ':' || *p == '/' || *p == '@'))
			return 0;
	}
	return 1;
}

/** artifact_names entry의 KEY=NAME 형식을 분해한다. */
static int
split_artifact_name_entry(const char *entry, char *key, size_t key_len, char *name,
    size_t name_len)
{
	const char *eq;
	size_t n;

	eq = entry ? strchr(entry, '=') : NULL;
	if (!eq || eq == entry || eq[1] == '\0')
		return 0;
	n = (size_t)(eq - entry);
	if (n + 1 > key_len || strlen(eq + 1) + 1 > name_len)
		return 0;
	memcpy(key, entry, n);
	key[n] = '\0';
	snprintf(name, name_len, "%s", eq + 1);
	return 1;
}

/** command tool spelling이 path separator를 포함하는지 확인한다. */
static int
tool_has_path_separator(const char *tool)
{
	return tool && (strchr(tool, '/') || strchr(tool, '\\'));
}

/** command tool spelling이 absolute path인지 확인한다. */
static int
tool_is_absolute_path(const char *tool)
{
	return tool && (tool[0] == '/' ||
	    (isalpha((unsigned char)tool[0]) && tool[1] == ':' &&
	    (tool[2] == '\\' || tool[2] == '/')));
}

/** PATH allowlist와 override key에 쓸 bare tool 이름인지 검사한다. */
static int
valid_tool_name(const char *tool)
{
	const unsigned char *p;

	if (!tool || !*tool || tool_has_path_separator(tool))
		return 0;
	for (p = (const unsigned char *)tool; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' || *p == '+'))
			return 0;
	}
	return 1;
}

/** build context boolean string을 기본값이 있는 boolean으로 낮춘다. */
static int
context_bool_enabled(const char *value, int default_value)
{
	if (!value || !*value)
		return default_value;
	return strcmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
	    strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
}

/** tool override entry의 NAME=PATH를 분리한다. */
static int
split_tool_override(const char *entry, char *name, size_t name_len, char *value,
    size_t value_len)
{
	const char *eq;
	size_t n;

	if (!entry)
		return 0;
	eq = strchr(entry, '=');
	if (!eq || eq == entry || eq[1] == '\0')
		return 0;
	n = (size_t)(eq - entry);
	if (n + 1 > name_len || strlen(eq + 1) + 1 > value_len)
		return 0;
	memcpy(name, entry, n);
	name[n] = '\0';
	snprintf(value, value_len, "%s", eq + 1);
	return 1;
}

/** response_files build context 값이 QStar v1 policy 안에 있는지 검사한다. */
static int
valid_response_files_value(const char *value)
{
	return !value || !*value || strcmp(value, "auto") == 0 ||
	    strcmp(value, "on") == 0 || strcmp(value, "off") == 0 ||
	    strcmp(value, "true") == 0 || strcmp(value, "false") == 0 ||
	    strcmp(value, "1") == 0 || strcmp(value, "0") == 0;
}

/** response_style build context 값이 QStar v1 policy 안에 있는지 검사한다. */
static int
valid_response_style_value(const char *value)
{
	return !value || !*value || strcmp(value, "posix") == 0 ||
	    strcmp(value, "windows") == 0 || strcmp(value, "msvc") == 0;
}

/** QStar build context 입력을 검증한다. */
int
qstar_graph_validate_build_context(struct qstar_graph *graph)
{
	size_t i;

	if (graph->build_context.sysroot && *graph->build_context.sysroot &&
	    !valid_build_context_path(graph->build_context.sysroot))
		return qstar_set_error(graph, "qstar: build context sysroot must be absolute or workspace-relative");
	if (graph->build_context.resource_dir && *graph->build_context.resource_dir &&
	    !valid_build_context_path(graph->build_context.resource_dir))
		return qstar_set_error(graph,
		    "qstar: build context resource_dir must be absolute or workspace-relative");
	if (!valid_build_context_bool(graph->build_context.allow_absolute_tools))
		return qstar_set_error(graph,
		    "qstar: build context allow_absolute_tools must be true, false, 1, 0, yes, no, on, or off");
	for (i = 0; i < graph->build_context.include_dirs.len; i++) {
		if (!valid_build_context_path(graph->build_context.include_dirs.items[i]))
			return qstar_set_error(graph,
			    "qstar: build context include_dirs entry '%s' must be absolute or workspace-relative",
			    graph->build_context.include_dirs.items[i]);
	}
	for (i = 0; i < graph->build_context.lib_dirs.len; i++) {
		if (!valid_build_context_path(graph->build_context.lib_dirs.items[i]))
			return qstar_set_error(graph,
			    "qstar: build context lib_dirs entry '%s' must be absolute or workspace-relative",
			    graph->build_context.lib_dirs.items[i]);
	}
	for (i = 0; i < graph->build_context.artifact_names.len; i++) {
		char key[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX];
		if (!split_artifact_name_entry(graph->build_context.artifact_names.items[i], key,
		    sizeof(key), name, sizeof(name)) || !valid_artifact_key(key) ||
		    !valid_artifact_name(name))
			return qstar_set_error(graph,
			    "qstar: build context artifact_names entry '%s' must be LABEL=FILENAME",
			    graph->build_context.artifact_names.items[i]);
	}
	for (i = 0; i < graph->build_context.path_tools.len; i++) {
		if (!valid_tool_name(graph->build_context.path_tools.items[i]))
			return qstar_set_error(graph,
			    "qstar: build context path_tools entry '%s' must be a bare tool name",
			    graph->build_context.path_tools.items[i]);
	}
	for (i = 0; i < graph->build_context.tool_overrides.len; i++) {
		char name[QSTAR_PATH_MAX], value[QSTAR_PATH_MAX];
		if (!split_tool_override(graph->build_context.tool_overrides.items[i], name,
		    sizeof(name), value, sizeof(value)) || !valid_tool_name(name) ||
		    !*value)
			return qstar_set_error(graph,
			    "qstar: build context tool_overrides entry '%s' must be NAME=VALUE",
			    graph->build_context.tool_overrides.items[i]);
		if (tool_is_absolute_path(value) &&
		    !context_bool_enabled(graph->build_context.allow_absolute_tools, 0))
			return qstar_set_error(graph,
			    "qstar: build context tool_overrides entry '%s' uses absolute path but allow_absolute_tools is not enabled",
			    graph->build_context.tool_overrides.items[i]);
		if (tool_has_path_separator(value) && !tool_is_absolute_path(value) &&
		    !qstar_path_is_package_relative(value))
			return qstar_set_error(graph,
			    "qstar: build context tool_overrides entry '%s' must resolve to package-relative, absolute, or PATH tool",
			    graph->build_context.tool_overrides.items[i]);
		if (!tool_has_path_separator(value) && !valid_tool_name(value))
			return qstar_set_error(graph,
			    "qstar: build context tool_overrides entry '%s' has invalid PATH tool",
			    graph->build_context.tool_overrides.items[i]);
	}
	if (!valid_response_files_value(graph->build_context.response_files))
		return qstar_set_error(graph,
		    "qstar: build context response_files must be auto, on, off, true, false, 1, or 0");
	if (!valid_response_style_value(graph->build_context.response_style))
		return qstar_set_error(graph,
		    "qstar: build context response_style must be posix, windows, or msvc");
	return 0;
}

/** target label을 build output directory 아래 파일명에 안전한 이름으로 바꾼다. */
void
qstar_mangle_label(const char *label, char *dst, size_t dstlen)
{
	size_t i;
	unsigned char c;

	if (!dstlen)
		return;
	for (i = 0; label[i] && i + 1 < dstlen; i++) {
		c = (unsigned char)label[i];
		dst[i] = isalnum(c) ? (char)c : '_';
	}
	dst[i] = '\0';
}

/** compile object output path를 deterministic package-relative path로 만든다. */
int
qstar_graph_object_output_path(const struct qstar_graph *graph,
    const struct qstar_target *target, size_t index, char *dst, size_t dstlen)
{
	char owner[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX];
	int n;

	qstar_mangle_label(target->label, owner, sizeof(owner));
	n = snprintf(sub, sizeof(sub), "out/%s/obj%zu.o", owner, index);
	return n >= 0 && (size_t)n < sizeof(sub) ?
	    qstar_graph_build_path(graph, sub, dst, dstlen) : -1;
}

/** compile depfile output path를 deterministic package-relative path로 만든다. */
int
qstar_graph_depfile_output_path(const struct qstar_graph *graph,
    const struct qstar_target *target, size_t index, char *dst, size_t dstlen)
{
	char owner[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX];
	int n;

	qstar_mangle_label(target->label, owner, sizeof(owner));
	n = snprintf(sub, sizeof(sub), "out/%s/obj%zu.d", owner, index);
	return n >= 0 && (size_t)n < sizeof(sub) ?
	    qstar_graph_build_path(graph, sub, dst, dstlen) : -1;
}

/** target artifact output path를 deterministic package-relative path로 만든다. */
int
qstar_artifact_output_path(const struct qstar_target *target, char *dst, size_t dstlen)
{
	return qstar_graph_artifact_output_path(NULL, target, dst, dstlen);
}

/** build context artifact_names에서 target label/name에 맞는 override를 찾는다. */
static const char *
build_context_artifact_name_for_target(const struct qstar_graph *graph,
    const struct qstar_target *target)
{
	char key[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX];
	size_t i;

	if (!graph || !target)
		return NULL;
	for (i = 0; i < graph->build_context.artifact_names.len; i++) {
		if (!split_artifact_name_entry(graph->build_context.artifact_names.items[i], key,
		    sizeof(key), name, sizeof(name)))
			continue;
		if (strcmp(key, target->label) == 0 || strcmp(key, target->name) == 0)
			return graph->build_context.artifact_names.items[i] + strlen(key) + 1;
	}
	return NULL;
}

/** build context/target artifact_name policy를 적용한 artifact output path를 만든다. */
int
qstar_graph_artifact_output_path(const struct qstar_graph *graph,
    const struct qstar_target *target, char *dst, size_t dstlen)
{
	char owner[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX];
	const struct qstar_target_rule_info *rule;
	const char *prefix, *suffix, *artifact_name;
	int n;

	qstar_mangle_label(target->label, owner, sizeof(owner));
	artifact_name = target->artifact_name && *target->artifact_name ?
	    target->artifact_name : build_context_artifact_name_for_target(graph, target);
	if (artifact_name && *artifact_name) {
		n = snprintf(sub, sizeof(sub), "out/%s/%s", owner, artifact_name);
		return n >= 0 && (size_t)n < sizeof(sub) ?
		    qstar_graph_build_path(graph, sub, dst, dstlen) : -1;
	}
	rule = qstar_target_rule_lookup(target->kind);
	prefix = rule ? rule->artifact_prefix : "";
	suffix = rule ? rule->artifact_suffix : "";
	if (strcmp(target->kind, "sharedlib") == 0) {
		if (qstar_toolchain_target_is_darwin(graph && graph->build_context.target ?
		    graph->build_context.target : "host"))
			suffix = ".dylib";
		else if (qstar_toolchain_target_is_windows(graph && graph->build_context.target ?
		    graph->build_context.target : "host")) {
			prefix = "";
			suffix = ".dll";
		} else {
			suffix = ".so";
		}
	}
	n = snprintf(sub, sizeof(sub), "out/%s/%s%s%s", owner, prefix, target->name, suffix);
	return n >= 0 && (size_t)n < sizeof(sub) ?
	    qstar_graph_build_path(graph, sub, dst, dstlen) : -1;
}
