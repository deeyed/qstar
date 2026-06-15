#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** 문자열 list에 값이 정확히 들어 있는지 확인한다. */
static int
string_list_contains(const struct qstar_string_list *list, const char *value)
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

/** build context tool override에서 command tool에 대응하는 replacement를 찾는다. */
static int
find_tool_override(const struct qstar_graph *graph, const char *tool, char *replacement,
    size_t replacement_len)
{
	char name[QSTAR_PATH_MAX], value[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < graph->build_context.tool_overrides.len; i++) {
		if (!split_tool_override(graph->build_context.tool_overrides.items[i], name,
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
qstar_external_tool_find_path_tool(const char *tool, char *dst, size_t dstlen)
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
qstar_external_tool_mode_is_package_input(const char *mode)
{
	return mode && (strcmp(mode, "package") == 0 ||
	    strcmp(mode, "override-package") == 0);
}

/** target/toolset 문맥에서 custom command PATH tool이 허용되는지 확인한다. */
static int
toolset_allows_path_tool(const struct qstar_graph *graph, const struct qstar_target *target,
    const char *tool)
{
	const struct qstar_toolset *toolset;
	size_t i;

	if (target && target->toolset && *target->toolset) {
		toolset = qstar_graph_find_toolset(graph, target->toolset);
		return toolset && string_list_contains(&toolset->path_tools, tool);
	}
	for (i = 0; graph && i < graph->toolset_len; i++) {
		if (string_list_contains(&graph->toolsets[i].path_tools, tool))
			return 1;
	}
	return 0;
}

/** target/toolset 문맥에서 absolute custom command가 허용되는지 확인한다. */
static int
toolset_allows_absolute_tool(const struct qstar_graph *graph,
    const struct qstar_target *target)
{
	const struct qstar_toolset *toolset;
	size_t i;

	if (target && target->toolset && *target->toolset) {
		toolset = qstar_graph_find_toolset(graph, target->toolset);
		return toolset && context_bool_enabled(toolset->allow_absolute_tools, 0);
	}
	for (i = 0; graph && i < graph->toolset_len; i++) {
		if (context_bool_enabled(graph->toolsets[i].allow_absolute_tools, 0))
			return 1;
	}
	return 0;
}

/** build context/toolset external tool policy로 custom_target 첫 argv를 실행 path로 해석한다. */
int
qstar_resolve_command_tool_for_target(const struct qstar_graph *graph,
    const struct qstar_target *target, const char *tool, char *resolved, size_t resolved_len,
    char *mode, size_t mode_len, char *error, size_t error_len)
{
	char override[QSTAR_PATH_MAX];
	int allow_absolute;

	allow_absolute = context_bool_enabled(graph->build_context.allow_absolute_tools, 0) ||
	    toolset_allows_absolute_tool(graph, target);

	if (!tool || !*tool) {
		snprintf(error, error_len, "qstar: generated action tool is empty");
		return -1;
	}
	if (valid_tool_name(tool) && find_tool_override(graph, tool, override,
	    sizeof(override))) {
		if (tool_is_absolute_path(override)) {
			if (!allow_absolute) {
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
		if (!allow_absolute) {
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
	if (!string_list_contains(&graph->build_context.path_tools, tool) &&
	    !toolset_allows_path_tool(graph, target, tool)) {
		snprintf(error, error_len,
		    "qstar: generated action PATH tool '%s' is not allowed by toolset path_tools",
		    tool);
		return -1;
	}
	return set_tool_resolution(resolved, resolved_len, mode, mode_len, tool, "path");
}

/** build context external tool policy로 custom_target 첫 argv를 실행 path로 해석한다. */
int
qstar_external_tool_resolve_command_tool(const struct qstar_graph *graph, const char *tool,
    char *resolved, size_t resolved_len, char *mode, size_t mode_len, char *error,
    size_t error_len)
{
	return qstar_resolve_command_tool_for_target(graph, NULL, tool, resolved,
	    resolved_len, mode, mode_len, error, error_len);
}
