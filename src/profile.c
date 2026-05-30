#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
		if (apply_profile_key(graph, trim(s), toml_scalar(eq + 1), !profile_file) < 0) {
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
		snprintf(resolved->cale, sizeof(resolved->cale), "cale");
		snprintf(resolved->ar, sizeof(resolved->ar), "ar");
		snprintf(resolved->linker, sizeof(resolved->linker), "cc");
		return 0;
	}
	if (strcmp(name, "clang") == 0) {
		snprintf(resolved->cc, sizeof(resolved->cc), "clang");
		snprintf(resolved->cale, sizeof(resolved->cale), "cale");
		snprintf(resolved->ar, sizeof(resolved->ar), "ar");
		snprintf(resolved->linker, sizeof(resolved->linker), "clang");
		return 0;
	}
	if (strcmp(name, "cale") == 0 || strcmp(name, "cale-sol") == 0) {
		snprintf(resolved->cc, sizeof(resolved->cc), "cale");
		snprintf(resolved->cale, sizeof(resolved->cale), "cale");
		snprintf(resolved->ar, sizeof(resolved->ar), "ar");
		snprintf(resolved->linker, sizeof(resolved->linker), "cale");
		return 0;
	}
	return qstar_set_error(graph, "qstar: unknown toolchain profile '%s'", name);
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

/** target artifact output path를 deterministic package-relative path로 만든다. */
int
qstar_artifact_output_path(const struct qstar_target *target, char *dst, size_t dstlen)
{
	char owner[QSTAR_PATH_MAX];
	const struct qstar_target_rule_info *rule;
	const char *prefix, *suffix;
	int n;

	qstar_mangle_label(target->label, owner, sizeof(owner));
	rule = qstar_target_rule_lookup(target->kind);
	prefix = rule ? rule->artifact_prefix : "";
	suffix = rule ? rule->artifact_suffix : "";
	n = snprintf(dst, dstlen, ".qstar/out/%s/%s%s%s", owner, prefix, target->name, suffix);
	return n >= 0 && (size_t)n < dstlen ? 0 : -1;
}
