#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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

/** consumer-context objectlib compile object output path를 만든다. */
int
qstar_graph_consumer_object_output_path(const struct qstar_graph *graph,
    const struct qstar_target *consumer, const struct qstar_target *objectlib,
    size_t index, char *dst, size_t dstlen)
{
	char consumer_owner[QSTAR_PATH_MAX], object_owner[QSTAR_PATH_MAX];
	char sub[QSTAR_PATH_MAX];
	int n;

	qstar_mangle_label(consumer->label, consumer_owner, sizeof(consumer_owner));
	qstar_mangle_label(objectlib->label, object_owner, sizeof(object_owner));
	n = snprintf(sub, sizeof(sub), "out/%s/objects/%s/obj%zu.o",
	    consumer_owner, object_owner, index);
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

/** consumer-context objectlib compile depfile output path를 만든다. */
int
qstar_graph_consumer_depfile_output_path(const struct qstar_graph *graph,
    const struct qstar_target *consumer, const struct qstar_target *objectlib,
    size_t index, char *dst, size_t dstlen)
{
	char consumer_owner[QSTAR_PATH_MAX], object_owner[QSTAR_PATH_MAX];
	char sub[QSTAR_PATH_MAX];
	int n;

	qstar_mangle_label(consumer->label, consumer_owner, sizeof(consumer_owner));
	qstar_mangle_label(objectlib->label, object_owner, sizeof(object_owner));
	n = snprintf(sub, sizeof(sub), "out/%s/objects/%s/obj%zu.d",
	    consumer_owner, object_owner, index);
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

static int
target_primary_artifact_filename(const struct qstar_graph *graph,
    const struct qstar_target *target, char *dst, size_t dstlen)
{
	const struct qstar_target_rule_info *rule;
	const char *prefix, *suffix, *artifact_name;
	int n;

	artifact_name = target->artifact_name;
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
	} else if ((strcmp(target->kind, "exe") == 0 ||
	    strcmp(target->kind, "test") == 0) &&
	    qstar_platform_is_windows(qstar_graph_platform(graph))) {
		suffix = ".exe";
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
	    strcmp(target->kind, "run_target") == 0 ||
	    strcmp(target->kind, "objectlib") == 0)
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
		if (target && target->kind && strcmp(target->kind, "objectlib") == 0)
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "target_file", target->label,
			    "qstar: qstar.target_file cannot reference objectlib target '%s' because object libraries have no artifact; consume it with objects = { ... }",
			    target->label);
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
