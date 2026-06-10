#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** directory에 qstar.workspace marker가 있는지 확인한다. */
static int
workspace_marker_exists(const char *dir)
{
	char path[QSTAR_PATH_MAX];
	FILE *f;

	if (qstar_path_join(dir, "qstar.workspace", path, sizeof(path)) < 0)
		return 0;
	f = fopen(path, "rb");
	if (!f)
		return 0;
	fclose(f);
	return 1;
}

/** qstar.lua 위치에서 위로 올라가며 profile root로 쓸 workspace를 찾는다. */
static int
discover_profile_root(const char *file_dir, char *root, size_t rootlen)
{
	char cur[QSTAR_PATH_MAX], parent[QSTAR_PATH_MAX];

	snprintf(cur, sizeof(cur), "%s", file_dir && *file_dir ? file_dir : ".");
	for (;;) {
		if (workspace_marker_exists(cur))
			return snprintf(root, rootlen, "%s", cur) < (int)rootlen ? 0 : -1;
		if (strcmp(cur, ".") == 0 || strcmp(cur, "/") == 0)
			break;
		if (qstar_dirname(cur, parent, sizeof(parent)) < 0 ||
		    strcmp(parent, cur) == 0)
			break;
		snprintf(cur, sizeof(cur), "%s", parent);
	}
	return snprintf(root, rootlen, "%s", file_dir && *file_dir ? file_dir : ".") <
	    (int)rootlen ? 0 : -1;
}

/** 앞뒤 공백을 제거한 문자열 시작 위치를 반환하고 오른쪽 공백은 in-place로 지운다. */
static char *
trim(char *s)
{
	char *end;

	while (*s && isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

/** TOML v1의 최소 quoted/unquoted scalar 값을 QStar 문자열로 정규화한다. */
static char *
toml_scalar(char *s)
{
	size_t n;

	s = trim(s);
	n = strlen(s);
	if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
		s[n - 1] = '\0';
		return s + 1;
	}
	return s;
}

/** TOML v2 profile list parser가 허용하는 quoted string list item을 추가한다. */
static int
toml_string_list(struct qstar_graph *graph, struct qstar_string_list *list, char *s)
{
	char *p, *q;

	s = trim(s);
	if (*s != '[')
		return qstar_set_error(graph, "qstar: profile list value must start with '['");
	p = s + 1;
	for (;;) {
		p = trim(p);
		if (*p == ']')
			return 0;
		if (*p != '"')
			return qstar_set_error(graph, "qstar: profile list items must be quoted strings");
		q = ++p;
		while (*q && *q != '"')
			q++;
		if (*q != '"')
			return qstar_set_error(graph, "qstar: unterminated profile list string");
		*q++ = '\0';
		if (qstar_string_list_push(list, p) < 0)
			return qstar_set_error(graph, "qstar: out of memory");
		q = trim(q);
		if (*q == ',') {
			p = q + 1;
			continue;
		}
		if (*q == ']')
			return 0;
		return qstar_set_error(graph, "qstar: malformed profile list");
	}
}

/** profile slot을 새 문자열로 교체한다. */
static int
profile_set(char **slot, const char *value)
{
	char *copy;

	copy = qstar_strdup(value);
	if (!copy)
		return -1;
	free(*slot);
	*slot = copy;
	return 0;
}

/** 최소 profile key/value를 graph profile에 반영한다. */
static int
apply_profile_key(struct qstar_graph *graph, const char *key, const char *value,
    int allow_name)
{
	if (strcmp(key, "profile") == 0) {
		if (!allow_name || graph->profile.name)
			return 0;
		return profile_set(&graph->profile.name, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	}
	if (strcmp(key, "target") == 0)
		return profile_set(&graph->profile.target, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "toolchain") == 0)
		return profile_set(&graph->profile.toolchain, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "stdlib") == 0 || strcmp(key, "stdlib_policy") == 0)
		return profile_set(&graph->profile.stdlib_policy, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "freestanding") == 0)
		return profile_set(&graph->profile.freestanding, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "arch") == 0)
		return profile_set(&graph->profile.arch, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "cpu") == 0)
		return profile_set(&graph->profile.cpu, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "abi") == 0)
		return profile_set(&graph->profile.abi, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "cc") == 0 || strcmp(key, "c_compiler") == 0)
		return profile_set(&graph->profile.cc, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "cxx") == 0 || strcmp(key, "cxx_compiler") == 0)
		return profile_set(&graph->profile.cxx, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "cale") == 0 || strcmp(key, "cale_compiler") == 0)
		return profile_set(&graph->profile.cale, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "ar") == 0 || strcmp(key, "archiver") == 0)
		return profile_set(&graph->profile.ar, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "linker") == 0)
		return profile_set(&graph->profile.linker, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "sysroot") == 0)
		return profile_set(&graph->profile.sysroot, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "resource_dir") == 0)
		return profile_set(&graph->profile.resource_dir, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "response_files") == 0 || strcmp(key, "rsp") == 0)
		return profile_set(&graph->profile.response_files, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "response_style") == 0 || strcmp(key, "rsp_style") == 0)
		return profile_set(&graph->profile.response_style, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "linker_script") == 0)
		return profile_set(&graph->profile.linker_script, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	if (strcmp(key, "allow_absolute_tools") == 0 ||
	    strcmp(key, "external_absolute_tools") == 0)
		return profile_set(&graph->profile.allow_absolute_tools, value) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	return 0;
}

/** profile schema v2 list key를 graph profile에 반영한다. */
static int
apply_profile_list(struct qstar_graph *graph, const char *key, char *value)
{
	if (strcmp(key, "include_dirs") == 0)
		return toml_string_list(graph, &graph->profile.include_dirs, value);
	if (strcmp(key, "lib_dirs") == 0)
		return toml_string_list(graph, &graph->profile.lib_dirs, value);
	if (strcmp(key, "link_options") == 0)
		return toml_string_list(graph, &graph->profile.link_options, value);
	if (strcmp(key, "defsyms") == 0)
		return toml_string_list(graph, &graph->profile.defsyms, value);
	if (strcmp(key, "artifact_names") == 0)
		return toml_string_list(graph, &graph->profile.artifact_names, value);
	if (strcmp(key, "path_tools") == 0 || strcmp(key, "external_tools") == 0)
		return toml_string_list(graph, &graph->profile.path_tools, value);
	if (strcmp(key, "tool_overrides") == 0)
		return toml_string_list(graph, &graph->profile.tool_overrides, value);
	return 0;
}

/** section header가 현재 profile에 대응하는 [profile.NAME]인지 검사한다. */
static int
section_matches_profile(const char *section, const char *profile)
{
	const char *name;

	if (strncmp(section, "profile.", 8) != 0)
		return 0;
	name = section + 8;
	return strcmp(name, profile && *profile ? profile : "default") == 0;
}

/** Cale.toml/.cale profile 파일의 read-only 최소 TOML key subset을 읽는다. */
static int
load_profile_toml(struct qstar_graph *graph, const char *path, int profile_file)
{
	FILE *f;
	char line[1024], *s, *eq, *hash, *section_end;
	char section[128];
	int active;

	f = fopen(path, "r");
	if (!f)
		return 0;
	section[0] = '\0';
	active = 1;
	while (fgets(line, sizeof(line), f)) {
		hash = strchr(line, '#');
		if (hash)
			*hash = '\0';
		s = trim(line);
		if (!*s)
			continue;
		if (*s == '[') {
			section_end = strchr(s, ']');
			if (!section_end) {
				fclose(f);
				return qstar_set_error(graph, "qstar: malformed profile section in '%s'",
				    path);
			}
			*section_end = '\0';
			snprintf(section, sizeof(section), "%s", s + 1);
			active = profile_file || section_matches_profile(section, graph->profile.name);
			continue;
		}
		if (!active)
			continue;
		eq = strchr(s, '=');
		if (!eq) {
			fclose(f);
			return qstar_set_error(graph, "qstar: malformed profile line in '%s'",
			    path);
		}
		*eq = '\0';
		s = trim(s);
		if (*trim(eq + 1) == '[') {
			if (apply_profile_list(graph, s, eq + 1) < 0) {
				fclose(f);
				return -1;
			}
			continue;
		}
		if (apply_profile_key(graph, s, toml_scalar(eq + 1), !profile_file) < 0) {
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	return 0;
}

/** Cale.toml과 .cale/profiles/<name>.toml의 최소 profile 입력을 읽어 graph에 반영한다. */
int
qstar_graph_load_profile_files(struct qstar_graph *graph, const char *qstar_file)
{
	char root[QSTAR_PATH_MAX], path[QSTAR_PATH_MAX], profile_path[QSTAR_PATH_MAX];
	const char *profile;

	if (qstar_dirname(qstar_file, root, sizeof(root)) < 0)
		return qstar_set_error(graph, "qstar: qstar file path too long");
	if (discover_profile_root(root, root, sizeof(root)) < 0)
		return qstar_set_error(graph, "qstar: profile root path too long");
	if (qstar_graph_set_package_root(graph, root) < 0)
		return -1;
	if (qstar_path_join(root, "Cale.toml", path, sizeof(path)) < 0)
		return qstar_set_error(graph, "qstar: profile path too long");
	if (load_profile_toml(graph, path, 0) < 0)
		return -1;
	profile = graph->profile.name && *graph->profile.name ? graph->profile.name : "default";
	if (snprintf(profile_path, sizeof(profile_path), ".cale/profiles/%s.toml", profile) >=
	    (int)sizeof(profile_path))
		return qstar_set_error(graph, "qstar: profile name is too long");
	if (qstar_path_join(root, profile_path, path, sizeof(path)) < 0)
		return qstar_set_error(graph, "qstar: profile path too long");
	return load_profile_toml(graph, path, 1);
}

static int
valid_profile_path(const char *path)
{
	return path && *path && (path[0] == '/' || qstar_path_is_package_relative(path));
}

/** profile linker_script가 실제 파일로 존재하는지 확인한다. */
static int
profile_linker_script_exists(const struct qstar_graph *graph, const char *path)
{
	char full[QSTAR_PATH_MAX];
	FILE *f;

	if (!path || !*path)
		return 1;
	if (path[0] == '/') {
		f = fopen(path, "rb");
	} else {
		if (qstar_path_join(graph->package_root ? graph->package_root : ".", path, full,
		    sizeof(full)) < 0)
			return 0;
		f = fopen(full, "rb");
	}
	if (!f)
		return 0;
	fclose(f);
	return 1;
}

/** profile boolean string이 QStar v3 policy 안에 있는지 검사한다. */
static int
valid_profile_bool(const char *value)
{
	return !value || !*value || strcmp(value, "true") == 0 ||
	    strcmp(value, "false") == 0 || strcmp(value, "1") == 0 ||
	    strcmp(value, "0") == 0 || strcmp(value, "yes") == 0 ||
	    strcmp(value, "no") == 0 || strcmp(value, "on") == 0 ||
	    strcmp(value, "off") == 0;
}

/** arch/cpu/abi profile token을 command-line flag에 안전한 문자로 제한한다. */
static int
valid_profile_token(const char *value)
{
	const unsigned char *p;

	if (!value || !*value)
		return 1;
	for (p = (const unsigned char *)value; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' || *p == '+'))
			return 0;
	}
	return 1;
}

/** defsym entry를 NAME=VALUE 형태로 제한한다. */
static int
valid_defsym(const char *value)
{
	const char *eq;

	if (!value || !*value)
		return 0;
	eq = strchr(value, '=');
	return eq && eq != value && eq[1] != '\0';
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

/** profile artifact_names entry의 target key를 제한한다. */
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

/** 문자열 list에 값이 정확히 들어 있는지 확인한다. */
static int
profile_list_contains(const struct qstar_string_list *list, const char *value)
{
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], value) == 0)
			return 1;
	}
	return 0;
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

/** profile boolean string을 기본값이 있는 boolean으로 낮춘다. */
static int
profile_bool_enabled(const char *value, int default_value)
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

/** profile tool override에서 command tool에 대응하는 replacement를 찾는다. */
static int
find_tool_override(const struct qstar_graph *graph, const char *tool, char *replacement,
    size_t replacement_len)
{
	char name[QSTAR_PATH_MAX], value[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < graph->profile.tool_overrides.len; i++) {
		if (!split_tool_override(graph->profile.tool_overrides.items[i], name,
		    sizeof(name), value, sizeof(value)))
			continue;
		if (strcmp(name, tool) == 0) {
			snprintf(replacement, replacement_len, "%s", value);
			return 1;
		}
	}
	return 0;
}

/** mode 문자열을 안전하게 복사한다. */
static int
set_tool_resolution(char *resolved, size_t resolved_len, char *mode, size_t mode_len,
    const char *resolved_value, const char *mode_value)
{
	return snprintf(resolved, resolved_len, "%s", resolved_value) < (int)resolved_len &&
	    snprintf(mode, mode_len, "%s", mode_value) < (int)mode_len ? 0 : -1;
}

/** PATH에서 실행 가능한 tool을 찾는다. */
int
qstar_profile_find_path_tool(const char *tool, char *dst, size_t dstlen)
{
	const char *path, *start, *end;
	char candidate[QSTAR_PATH_MAX];
	size_t dirlen;

	if (!valid_tool_name(tool))
		return 0;
	path = getenv("PATH");
	if (!path)
		return 0;
	start = path;
	for (;;) {
		end = strchr(start, ':');
		dirlen = end ? (size_t)(end - start) : strlen(start);
		if (dirlen == 0) {
			if (snprintf(candidate, sizeof(candidate), "./%s", tool) >=
			    (int)sizeof(candidate))
				return 0;
		} else {
			if (dirlen + 1 + strlen(tool) + 1 > sizeof(candidate))
				goto next;
			memcpy(candidate, start, dirlen);
			candidate[dirlen] = '/';
			snprintf(candidate + dirlen + 1, sizeof(candidate) - dirlen - 1,
			    "%s", tool);
		}
		if (access(candidate, X_OK) == 0)
			return snprintf(dst, dstlen, "%s", candidate) < (int)dstlen ? 1 : 0;
next:
		if (!end)
			break;
		start = end + 1;
	}
	return 0;
}

/** mode가 package-local file input으로 action key에 들어가야 하는지 본다. */
int
qstar_profile_tool_mode_is_package_input(const char *mode)
{
	return mode && (strcmp(mode, "package") == 0 ||
	    strcmp(mode, "override-package") == 0);
}

/** profile external tool policy로 custom_target 첫 argv를 실행 path로 해석한다. */
int
qstar_profile_resolve_command_tool(const struct qstar_graph *graph, const char *tool,
    char *resolved, size_t resolved_len, char *mode, size_t mode_len, char *error,
    size_t error_len)
{
	char override[QSTAR_PATH_MAX];

	if (!tool || !*tool) {
		snprintf(error, error_len, "qstar: generated action tool is empty");
		return -1;
	}
	if (valid_tool_name(tool) && find_tool_override(graph, tool, override,
	    sizeof(override))) {
		if (tool_is_absolute_path(override)) {
			if (!profile_bool_enabled(graph->profile.allow_absolute_tools, 0)) {
				snprintf(error, error_len,
				    "qstar: tool override for '%s' resolves to absolute path '%s' but allow_absolute_tools is not enabled",
				    tool, override);
				return -1;
			}
			return set_tool_resolution(resolved, resolved_len, mode, mode_len,
			    override, "override-absolute");
		}
		if (tool_has_path_separator(override)) {
			if (!qstar_path_is_package_relative(override)) {
				snprintf(error, error_len,
				    "qstar: tool override for '%s' must be package-relative, absolute, or PATH tool",
				    tool);
				return -1;
			}
			return set_tool_resolution(resolved, resolved_len, mode, mode_len,
			    override, "override-package");
		}
		if (!valid_tool_name(override)) {
			snprintf(error, error_len,
			    "qstar: tool override for '%s' has invalid PATH tool '%s'",
			    tool, override);
			return -1;
		}
		return set_tool_resolution(resolved, resolved_len, mode, mode_len,
		    override, "override-path");
	}
	if (tool_is_absolute_path(tool)) {
		if (!profile_bool_enabled(graph->profile.allow_absolute_tools, 0)) {
			snprintf(error, error_len,
			    "qstar: absolute generated action tool '%s' requires allow_absolute_tools=true",
			    tool);
			return -1;
		}
		return set_tool_resolution(resolved, resolved_len, mode, mode_len, tool,
		    "absolute");
	}
	if (tool_has_path_separator(tool)) {
		if (!qstar_path_is_package_relative(tool)) {
			snprintf(error, error_len,
			    "qstar: generated action tool '%s' must be package-relative",
			    tool);
			return -1;
		}
		return set_tool_resolution(resolved, resolved_len, mode, mode_len, tool,
		    "package");
	}
	if (!valid_tool_name(tool)) {
		snprintf(error, error_len, "qstar: generated action PATH tool '%s' is invalid",
		    tool);
		return -1;
	}
	if (!profile_list_contains(&graph->profile.path_tools, tool)) {
		snprintf(error, error_len,
		    "qstar: generated action PATH tool '%s' is not allowed by profile path_tools",
		    tool);
		return -1;
	}
	return set_tool_resolution(resolved, resolved_len, mode, mode_len, tool, "path");
}

/** response_files profile 값이 QStar v1 policy 안에 있는지 검사한다. */
static int
valid_response_files_value(const char *value)
{
	return !value || !*value || strcmp(value, "auto") == 0 ||
	    strcmp(value, "on") == 0 || strcmp(value, "off") == 0 ||
	    strcmp(value, "true") == 0 || strcmp(value, "false") == 0 ||
	    strcmp(value, "1") == 0 || strcmp(value, "0") == 0;
}

/** response_style profile 값이 QStar v1 policy 안에 있는지 검사한다. */
static int
valid_response_style_value(const char *value)
{
	return !value || !*value || strcmp(value, "posix") == 0 ||
	    strcmp(value, "windows") == 0 || strcmp(value, "msvc") == 0;
}

/** QStar profile schema v2 입력을 검증한다. */
int
qstar_graph_validate_profile(struct qstar_graph *graph)
{
	size_t i;

	if (graph->profile.sysroot && *graph->profile.sysroot &&
	    !valid_profile_path(graph->profile.sysroot))
		return qstar_set_error(graph, "qstar: profile sysroot must be absolute or workspace-relative");
	if (graph->profile.resource_dir && *graph->profile.resource_dir &&
	    !valid_profile_path(graph->profile.resource_dir))
		return qstar_set_error(graph,
		    "qstar: profile resource_dir must be absolute or workspace-relative");
	if (graph->profile.linker_script && *graph->profile.linker_script &&
	    !valid_profile_path(graph->profile.linker_script))
		return qstar_set_error(graph,
		    "qstar: profile linker_script must be absolute or workspace-relative");
	if (!profile_linker_script_exists(graph, graph->profile.linker_script))
		return qstar_set_error(graph,
		    "qstar: profile linker_script '%s' does not exist",
		    graph->profile.linker_script);
	if (!valid_profile_bool(graph->profile.freestanding))
		return qstar_set_error(graph,
		    "qstar: profile freestanding must be true, false, 1, 0, yes, no, on, or off");
	if (!valid_profile_bool(graph->profile.allow_absolute_tools))
		return qstar_set_error(graph,
		    "qstar: profile allow_absolute_tools must be true, false, 1, 0, yes, no, on, or off");
	if (!valid_profile_token(graph->profile.arch))
		return qstar_set_error(graph, "qstar: profile arch contains unsupported characters");
	if (!valid_profile_token(graph->profile.cpu))
		return qstar_set_error(graph, "qstar: profile cpu contains unsupported characters");
	if (!valid_profile_token(graph->profile.abi))
		return qstar_set_error(graph, "qstar: profile abi contains unsupported characters");
	for (i = 0; i < graph->profile.include_dirs.len; i++) {
		if (!valid_profile_path(graph->profile.include_dirs.items[i]))
			return qstar_set_error(graph,
			    "qstar: profile include_dirs entry '%s' must be absolute or workspace-relative",
			    graph->profile.include_dirs.items[i]);
	}
	for (i = 0; i < graph->profile.lib_dirs.len; i++) {
		if (!valid_profile_path(graph->profile.lib_dirs.items[i]))
			return qstar_set_error(graph,
			    "qstar: profile lib_dirs entry '%s' must be absolute or workspace-relative",
			    graph->profile.lib_dirs.items[i]);
	}
	for (i = 0; i < graph->profile.defsyms.len; i++) {
		if (!valid_defsym(graph->profile.defsyms.items[i]))
			return qstar_set_error(graph,
			    "qstar: profile defsyms entry '%s' must be NAME=VALUE",
			    graph->profile.defsyms.items[i]);
	}
	for (i = 0; i < graph->profile.artifact_names.len; i++) {
		char key[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX];
		if (!split_artifact_name_entry(graph->profile.artifact_names.items[i], key,
		    sizeof(key), name, sizeof(name)) || !valid_artifact_key(key) ||
		    !valid_artifact_name(name))
			return qstar_set_error(graph,
			    "qstar: profile artifact_names entry '%s' must be LABEL=FILENAME",
			    graph->profile.artifact_names.items[i]);
	}
	for (i = 0; i < graph->profile.path_tools.len; i++) {
		if (!valid_tool_name(graph->profile.path_tools.items[i]))
			return qstar_set_error(graph,
			    "qstar: profile path_tools entry '%s' must be a bare tool name",
			    graph->profile.path_tools.items[i]);
	}
	for (i = 0; i < graph->profile.tool_overrides.len; i++) {
		char name[QSTAR_PATH_MAX], value[QSTAR_PATH_MAX];
		if (!split_tool_override(graph->profile.tool_overrides.items[i], name,
		    sizeof(name), value, sizeof(value)) || !valid_tool_name(name) ||
		    !*value)
			return qstar_set_error(graph,
			    "qstar: profile tool_overrides entry '%s' must be NAME=VALUE",
			    graph->profile.tool_overrides.items[i]);
		if (tool_is_absolute_path(value) &&
		    !profile_bool_enabled(graph->profile.allow_absolute_tools, 0))
			return qstar_set_error(graph,
			    "qstar: profile tool_overrides entry '%s' uses absolute path but allow_absolute_tools is not enabled",
			    graph->profile.tool_overrides.items[i]);
		if (tool_has_path_separator(value) && !tool_is_absolute_path(value) &&
		    !qstar_path_is_package_relative(value))
			return qstar_set_error(graph,
			    "qstar: profile tool_overrides entry '%s' must resolve to package-relative, absolute, or PATH tool",
			    graph->profile.tool_overrides.items[i]);
		if (!tool_has_path_separator(value) && !valid_tool_name(value))
			return qstar_set_error(graph,
			    "qstar: profile tool_overrides entry '%s' has invalid PATH tool",
			    graph->profile.tool_overrides.items[i]);
	}
	if (!valid_response_files_value(graph->profile.response_files))
		return qstar_set_error(graph,
		    "qstar: profile response_files must be auto, on, off, true, false, 1, or 0");
	if (!valid_response_style_value(graph->profile.response_style))
		return qstar_set_error(graph,
		    "qstar: profile response_style must be posix, windows, or msvc");
	return 0;
}

/** target triple에서 Windows 계열 여부를 보수적으로 판정한다. */
static int
profile_target_is_windows(const char *target)
{
	return target && (strstr(target, "windows") || strstr(target, "mingw") ||
	    strstr(target, "msvc"));
}

/** target triple에서 MSVC command-line 계열 여부를 판정한다. */
static int
profile_target_is_msvc(const char *target)
{
	return target && strstr(target, "msvc");
}

/** response_files profile 값을 boolean capability로 낮춘다. */
static int
profile_response_files_enabled(const char *value, int default_value)
{
	if (!value || !*value || strcmp(value, "auto") == 0)
		return default_value;
	if (strcmp(value, "off") == 0 || strcmp(value, "false") == 0 ||
	    strcmp(value, "0") == 0)
		return 0;
	return 1;
}

/** target/profile 입력을 합쳐 host/clang/cale toolchain v1을 결정한다. */
int
qstar_resolve_toolchain(struct qstar_graph *graph, const struct qstar_target *target,
    struct qstar_resolved_toolchain *resolved)
{
	const char *name, *stdlib_policy, *triple;

	memset(resolved, 0, sizeof(*resolved));
	name = target->toolchain && strcmp(target->toolchain, "host") != 0 ?
	    target->toolchain : graph->profile.toolchain;
	if (!name || !*name)
		name = target->toolchain && *target->toolchain ? target->toolchain : "host";
	stdlib_policy = target->stdlib_policy && strcmp(target->stdlib_policy, "system") != 0 ?
	    target->stdlib_policy : graph->profile.stdlib_policy;
	if (!stdlib_policy || !*stdlib_policy)
		stdlib_policy = target->stdlib_policy && *target->stdlib_policy ?
		    target->stdlib_policy : "system";
	triple = graph->profile.target && *graph->profile.target ? graph->profile.target : "host";
	snprintf(resolved->name, sizeof(resolved->name), "%s", name);
	snprintf(resolved->target, sizeof(resolved->target), "%s", triple);
	snprintf(resolved->stdlib_policy, sizeof(resolved->stdlib_policy), "%s", stdlib_policy);
	snprintf(resolved->resolver, sizeof(resolved->resolver), "builtin-v1");
	if (strcmp(name, "host") == 0 || strcmp(name, "default") == 0) {
		snprintf(resolved->cc, sizeof(resolved->cc), "cc");
		snprintf(resolved->cxx, sizeof(resolved->cxx), "c++");
		snprintf(resolved->cale, sizeof(resolved->cale), "cale");
		snprintf(resolved->ar, sizeof(resolved->ar), "ar");
		snprintf(resolved->linker, sizeof(resolved->linker), "cc");
	} else if (strcmp(name, "clang") == 0) {
		snprintf(resolved->cc, sizeof(resolved->cc), "clang");
		snprintf(resolved->cxx, sizeof(resolved->cxx), "clang++");
		snprintf(resolved->cale, sizeof(resolved->cale), "cale");
		snprintf(resolved->ar, sizeof(resolved->ar), "ar");
		snprintf(resolved->linker, sizeof(resolved->linker), "clang");
	} else if (strcmp(name, "cale") == 0 || strcmp(name, "cale-sol") == 0) {
		snprintf(resolved->cc, sizeof(resolved->cc), "cale");
		snprintf(resolved->cxx, sizeof(resolved->cxx), "c++");
		snprintf(resolved->cale, sizeof(resolved->cale), "cale");
		snprintf(resolved->ar, sizeof(resolved->ar), "ar");
		snprintf(resolved->linker, sizeof(resolved->linker), "cale");
	} else {
		return qstar_set_error(graph, "qstar: unknown toolchain profile '%s'", name);
	}
	if (graph->profile.cc && *graph->profile.cc)
		snprintf(resolved->cc, sizeof(resolved->cc), "%s", graph->profile.cc);
	if (graph->profile.cxx && *graph->profile.cxx)
		snprintf(resolved->cxx, sizeof(resolved->cxx), "%s", graph->profile.cxx);
	if (graph->profile.cale && *graph->profile.cale)
		snprintf(resolved->cale, sizeof(resolved->cale), "%s", graph->profile.cale);
	if (graph->profile.ar && *graph->profile.ar)
		snprintf(resolved->ar, sizeof(resolved->ar), "%s", graph->profile.ar);
	if (graph->profile.linker && *graph->profile.linker)
		snprintf(resolved->linker, sizeof(resolved->linker), "%s", graph->profile.linker);
	if (graph->profile.sysroot && *graph->profile.sysroot)
		snprintf(resolved->sysroot, sizeof(resolved->sysroot), "%s", graph->profile.sysroot);
	if (graph->profile.resource_dir && *graph->profile.resource_dir)
		snprintf(resolved->resource_dir, sizeof(resolved->resource_dir), "%s",
		    graph->profile.resource_dir);
	resolved->response_files =
	    profile_response_files_enabled(graph->profile.response_files, 1);
	if (graph->profile.response_style && *graph->profile.response_style)
		snprintf(resolved->response_style, sizeof(resolved->response_style), "%s",
		    graph->profile.response_style);
	else if (profile_target_is_msvc(resolved->target))
		snprintf(resolved->response_style, sizeof(resolved->response_style), "msvc");
	else if (profile_target_is_windows(resolved->target))
		snprintf(resolved->response_style, sizeof(resolved->response_style),
		    "windows");
	else
		snprintf(resolved->response_style, sizeof(resolved->response_style), "posix");
	snprintf(resolved->resolver, sizeof(resolved->resolver), "profile-schema-v2");
	return 0;
}

/** target label을 .qstar/out 아래 파일명에 안전한 이름으로 바꾼다. */
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
qstar_object_output_path(const struct qstar_target *target, size_t index, char *dst,
    size_t dstlen)
{
	char owner[QSTAR_PATH_MAX];
	int n;

	qstar_mangle_label(target->label, owner, sizeof(owner));
	n = snprintf(dst, dstlen, ".qstar/out/%s/obj%zu.o", owner, index);
	return n >= 0 && (size_t)n < dstlen ? 0 : -1;
}

/** compile depfile output path를 deterministic package-relative path로 만든다. */
int
qstar_depfile_output_path(const struct qstar_target *target, size_t index, char *dst,
    size_t dstlen)
{
	char owner[QSTAR_PATH_MAX];
	int n;

	qstar_mangle_label(target->label, owner, sizeof(owner));
	n = snprintf(dst, dstlen, ".qstar/out/%s/obj%zu.d", owner, index);
	return n >= 0 && (size_t)n < dstlen ? 0 : -1;
}

/** target artifact output path를 deterministic package-relative path로 만든다. */
int
qstar_artifact_output_path(const struct qstar_target *target, char *dst, size_t dstlen)
{
	return qstar_graph_artifact_output_path(NULL, target, dst, dstlen);
}

/** profile artifact_names에서 target label/name에 맞는 override를 찾는다. */
static const char *
profile_artifact_name_for_target(const struct qstar_graph *graph,
    const struct qstar_target *target)
{
	char key[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX];
	size_t i;

	if (!graph || !target)
		return NULL;
	for (i = 0; i < graph->profile.artifact_names.len; i++) {
		if (!split_artifact_name_entry(graph->profile.artifact_names.items[i], key,
		    sizeof(key), name, sizeof(name)))
			continue;
		if (strcmp(key, target->label) == 0 || strcmp(key, target->name) == 0)
			return graph->profile.artifact_names.items[i] + strlen(key) + 1;
	}
	return NULL;
}

/** profile/target artifact_name policy를 적용한 artifact output path를 만든다. */
int
qstar_graph_artifact_output_path(const struct qstar_graph *graph,
    const struct qstar_target *target, char *dst, size_t dstlen)
{
	char owner[QSTAR_PATH_MAX];
	const struct qstar_target_rule_info *rule;
	const char *prefix, *suffix, *artifact_name;
	int n;

	qstar_mangle_label(target->label, owner, sizeof(owner));
	artifact_name = target->artifact_name && *target->artifact_name ?
	    target->artifact_name : profile_artifact_name_for_target(graph, target);
	if (artifact_name && *artifact_name) {
		n = snprintf(dst, dstlen, ".qstar/out/%s/%s", owner, artifact_name);
		return n >= 0 && (size_t)n < dstlen ? 0 : -1;
	}
	rule = qstar_target_rule_lookup(target->kind);
	prefix = rule ? rule->artifact_prefix : "";
	suffix = rule ? rule->artifact_suffix : "";
	n = snprintf(dst, dstlen, ".qstar/out/%s/%s%s%s", owner, prefix, target->name, suffix);
	return n >= 0 && (size_t)n < dstlen ? 0 : -1;
}
