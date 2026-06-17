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

static int
artifact_path_with_filename(const struct qstar_graph *graph,
    const struct qstar_target *target, const char *filename, char *dst, size_t dstlen)
{
	char owner[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX];
	int n;

	qstar_mangle_label(target->label, owner, sizeof(owner));
	n = snprintf(sub, sizeof(sub), "out/%s/%s", owner, filename);
	return n >= 0 && (size_t)n < sizeof(sub) ?
	    qstar_graph_build_path(graph, sub, dst, dstlen) : -1;
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

static int
target_primary_artifact_filename(const struct qstar_graph *graph,
    const struct qstar_target *target, char *dst, size_t dstlen)
{
	const struct qstar_target_rule_info *rule;
	const char *prefix, *suffix, *artifact_name;
	int n;

	artifact_name = target->artifact_name && *target->artifact_name ?
	    target->artifact_name : build_context_artifact_name_for_target(graph, target);
	if (artifact_name && *artifact_name) {
		n = snprintf(dst, dstlen, "%s", artifact_name);
		return n >= 0 && (size_t)n < dstlen ? 0 : -1;
	}
	rule = qstar_target_rule_lookup(target->kind);
	prefix = rule ? rule->artifact_prefix : "";
	suffix = rule ? rule->artifact_suffix : "";
	if (strcmp(target->kind, "sharedlib") == 0) {
		if (qstar_platform_is_darwin(qstar_graph_platform(graph)))
			suffix = ".dylib";
		else if (qstar_platform_is_windows(qstar_graph_platform(graph))) {
			prefix = "";
			suffix = ".dll";
		} else {
			suffix = ".so";
		}
	}
	n = snprintf(dst, dstlen, "%s%s%s", prefix, target->name, suffix);
	return n >= 0 && (size_t)n < dstlen ? 0 : -1;
}

static int
replace_extension(const char *filename, const char *suffix, char *dst, size_t dstlen)
{
	const char *dot;
	size_t n;
	int rc;

	dot = strrchr(filename, '.');
	n = dot && dot != filename ? (size_t)(dot - filename) : strlen(filename);
	if (n + strlen(suffix) + 1 > dstlen)
		return -1;
	memcpy(dst, filename, n);
	rc = snprintf(dst + n, dstlen - n, "%s", suffix);
	return rc >= 0 && (size_t)rc < dstlen - n ? 0 : -1;
}

static int
push_artifact(struct qstar_target_artifact_map *map, const char *id, const char *role,
    const char *path, const char *install_dir, int primary, int installable)
{
	struct qstar_target_artifact *artifact;
	int n;

	if (map->len >= sizeof(map->items) / sizeof(map->items[0]))
		return -1;
	artifact = &map->items[map->len++];
	n = snprintf(artifact->id, sizeof(artifact->id), "%s", id);
	if (n < 0 || (size_t)n >= sizeof(artifact->id))
		return -1;
	n = snprintf(artifact->role, sizeof(artifact->role), "%s", role);
	if (n < 0 || (size_t)n >= sizeof(artifact->role))
		return -1;
	n = snprintf(artifact->path, sizeof(artifact->path), "%s", path);
	if (n < 0 || (size_t)n >= sizeof(artifact->path))
		return -1;
	n = snprintf(artifact->install_dir, sizeof(artifact->install_dir), "%s",
	    install_dir ? install_dir : "");
	if (n < 0 || (size_t)n >= sizeof(artifact->install_dir))
		return -1;
	artifact->primary = primary;
	artifact->installable = installable;
	return 0;
}

/** target이 생산하는 artifact map을 platform context 기준으로 계산한다. */
int
qstar_graph_target_artifact_map(const struct qstar_graph *graph,
    const struct qstar_target *target, struct qstar_target_artifact_map *map)
{
	char filename[QSTAR_PATH_MAX], import_filename[QSTAR_PATH_MAX];
	char path[QSTAR_PATH_MAX], import_path[QSTAR_PATH_MAX];
	const char *kind;
	int installable;

	memset(map, 0, sizeof(*map));
	if (!target || !target->kind || strcmp(target->kind, "group") == 0 ||
	    strcmp(target->kind, "run_target") == 0)
		return 0;
	if (target_primary_artifact_filename(graph, target, filename, sizeof(filename)) < 0 ||
	    artifact_path_with_filename(graph, target, filename, path, sizeof(path)) < 0)
		return -1;
	kind = target->kind;
	installable = qstar_target_is_installable(target);
	if (strcmp(kind, "sharedlib") == 0) {
		if (qstar_platform_is_windows(qstar_graph_platform(graph))) {
			if (push_artifact(map, "runtime", "sharedlib", path, "bin", 1,
			    installable) < 0 ||
			    replace_extension(filename, ".lib", import_filename,
			    sizeof(import_filename)) < 0 ||
			    artifact_path_with_filename(graph, target, import_filename,
			    import_path, sizeof(import_path)) < 0 ||
			    push_artifact(map, "import_lib", "import_lib", import_path,
			    "lib", 0, installable) < 0)
				return -1;
			return 0;
		}
		return push_artifact(map, "runtime", "sharedlib", path, "lib", 1,
		    installable);
	}
	if (strcmp(kind, "exe") == 0 || strcmp(kind, "test") == 0)
		return push_artifact(map, "runtime", "exe", path, "bin", 1,
		    installable);
	if (strcmp(kind, "staticlib") == 0)
		return push_artifact(map, "archive", "staticlib", path, "lib", 1,
		    installable);
	return push_artifact(map, "primary", kind, path, "", 1, installable);
}

static void
known_artifact_selectors(const struct qstar_target_artifact_map *map, char *dst,
    size_t dstlen)
{
	size_t i, used;
	int n;

	if (!dstlen)
		return;
	dst[0] = '\0';
	used = 0;
	for (i = 0; i < map->len; i++) {
		n = snprintf(dst + used, dstlen - used, "%s%s", i ? ", " : "",
		    map->items[i].id);
		if (n < 0 || (size_t)n >= dstlen - used) {
			dst[dstlen - 1] = '\0';
			return;
		}
		used += (size_t)n;
	}
}

/** target artifact selector를 deterministic package-relative path로 해석한다. */
int
qstar_graph_target_artifact_path(struct qstar_graph *graph,
    const struct qstar_target *target, const char *artifact, char *dst, size_t dstlen)
{
	struct qstar_target_artifact_map map;
	char known[256];
	size_t i;

	if (qstar_graph_target_artifact_map(graph, target, &map) < 0)
		return graph ? qstar_set_error(graph, "qstar: target artifact path too long") : -1;
	for (i = 0; i < map.len; i++) {
		if ((!artifact || !*artifact || strcmp(artifact, "primary") == 0) &&
		    map.items[i].primary)
			break;
		if (artifact && *artifact && strcmp(artifact, map.items[i].id) == 0)
			break;
	}
	if (i == map.len) {
		if (!graph)
			return -1;
		known_artifact_selectors(&map, known, sizeof(known));
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "target_file", target->label,
		    "qstar: target_file artifact selector '%s' is unknown for target '%s'; known artifacts: %s",
		    artifact && *artifact ? artifact : "<primary>", target->label,
		    known[0] ? known : "<none>");
	}
	if (snprintf(dst, dstlen, "%s", map.items[i].path) >= (int)dstlen)
		return graph ? qstar_set_error(graph, "qstar: target artifact path too long") : -1;
	return 0;
}

/** target final action이 생산하는 모든 artifact path를 반환한다. */
int
qstar_graph_target_artifact_outputs(struct qstar_graph *graph,
    const struct qstar_target *target, struct qstar_string_list *outputs)
{
	struct qstar_target_artifact_map map;
	size_t i;

	memset(outputs, 0, sizeof(*outputs));
	if (qstar_graph_target_artifact_map(graph, target, &map) < 0)
		return qstar_set_error(graph, "qstar: target artifact path too long");
	for (i = 0; i < map.len; i++) {
		if (qstar_string_list_push(outputs, map.items[i].path) < 0) {
			qstar_string_list_free(outputs);
			return qstar_set_error(graph, "qstar: out of memory");
		}
	}
	return 0;
}

/** dependency를 link할 때 사용할 artifact path를 platform policy 기준으로 반환한다. */
int
qstar_graph_target_link_artifact_path(struct qstar_graph *graph,
    const struct qstar_target *target, const char *platform, char *dst, size_t dstlen)
{
	const char *selector;

	selector = strcmp(target->kind, "sharedlib") == 0 &&
	    qstar_platform_is_windows(platform) ? "import_lib" : NULL;
	return qstar_graph_target_artifact_path(graph, target, selector, dst, dstlen);
}

/** build context/target artifact_name policy를 적용한 primary artifact output path를 만든다. */
int
qstar_graph_artifact_output_path(const struct qstar_graph *graph,
    const struct qstar_target *target, char *dst, size_t dstlen)
{
	return qstar_graph_target_artifact_path((struct qstar_graph *)graph, target, NULL,
	    dst, dstlen);
}
