#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#define QSTAR_PATH_LIST_SEPARATOR ';'
#else
#define QSTAR_PATH_LIST_SEPARATOR ':'
#endif

#define QSTAR_CAS_MAGIC "qstar-local-cas-v1"
#define QSTAR_CAS_HASH_INIT 1469598103934665603ULL
#define QSTAR_CAS_HASH_PRIME 1099511628211ULL

static unsigned long long
cache_hash_bytes(unsigned long long h, const void *data, size_t len)
{
	const unsigned char *p = data;
	size_t i;

	for (i = 0; i < len; i++) {
		h ^= p[i];
		h *= QSTAR_CAS_HASH_PRIME;
	}
	return h;
}

static unsigned long long
cache_hash_string(unsigned long long h, const char *s)
{
	s = s ? s : "";
	h = cache_hash_bytes(h, s, strlen(s));
	return cache_hash_bytes(h, "\n", 1);
}

static void
cache_format_hash(unsigned long long h, char *dst, size_t dstlen)
{
	snprintf(dst, dstlen, "%016llx", h);
}

static int
cache_full_path(const struct qstar_graph *graph, const char *rel, char *dst,
    size_t dstlen)
{
	const char *root;

	if (!rel || !*rel)
		return -1;
	if (rel[0] == '/')
		return snprintf(dst, dstlen, "%s", rel) < (int)dstlen ? 0 : -1;
	root = graph->package_root && *graph->package_root ? graph->package_root : ".";
	return qstar_path_join(root, rel, dst, dstlen);
}

static int
cache_mkdir_p(const char *path)
{
	char tmp[QSTAR_PATH_MAX];
	char *p;

	if (!path || !*path || snprintf(tmp, sizeof(tmp), "%s", path) >=
	    (int)sizeof(tmp))
		return -1;
	for (p = tmp + 1; *p; p++) {
		if (*p != '/' && *p != '\\')
			continue;
		*p = '\0';
		if (qstar_platform_mkdir(tmp, 0777) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return qstar_platform_mkdir(tmp, 0777) == 0 || errno == EEXIST ? 0 : -1;
}

static int
cache_mkdir_parent(const char *path)
{
	char dir[QSTAR_PATH_MAX];

	return qstar_dirname(path, dir, sizeof(dir)) == 0 ? cache_mkdir_p(dir) : -1;
}

static int
cache_file_hash(const char *path, unsigned long long *hash, unsigned long long *size,
    unsigned int *mode)
{
	unsigned char buf[65536];
	struct stat st;
	FILE *f;
	size_t n;
	unsigned long long h;

	if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
		return -1;
	f = fopen(path, "rb");
	if (!f)
		return -1;
	h = QSTAR_CAS_HASH_INIT;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		h = cache_hash_bytes(h, buf, n);
	if (ferror(f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	*hash = h;
	if (size)
		*size = (unsigned long long)st.st_size;
	if (mode)
		*mode = (unsigned int)(st.st_mode & 0777);
	return 0;
}

static int
cache_copy_file(const char *src, const char *dst, unsigned int mode)
{
	unsigned char buf[65536];
	FILE *in, *out;
	size_t n;

	in = fopen(src, "rb");
	if (!in)
		return -1;
	if (cache_mkdir_parent(dst) < 0 || !(out = fopen(dst, "wb"))) {
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			fclose(in);
			fclose(out);
			return -1;
		}
	}
	if (ferror(in) || fclose(out) != 0) {
		fclose(in);
		return -1;
	}
	fclose(in);
#if !defined(_WIN32)
	chmod(dst, (mode_t)mode);
#else
	(void)mode;
#endif
	return 0;
}

static int
cache_executable_candidate(const char *candidate)
{
	struct stat st;

	return candidate && stat(candidate, &st) == 0 && S_ISREG(st.st_mode);
}

static int
cache_resolve_tool(const struct qstar_graph *graph, const char *tool, char *dst,
    size_t dstlen)
{
	const char *path, *start, *end;
	char dir[QSTAR_PATH_MAX], candidate[QSTAR_PATH_MAX];
	size_t len;

	if (!tool || !*tool)
		return -1;
	if (strchr(tool, '/') || strchr(tool, '\\')) {
		if (cache_full_path(graph, tool, dst, dstlen) < 0)
			return -1;
		return cache_executable_candidate(dst) ? 0 : -1;
	}
	path = getenv("PATH");
	if (!path)
		return -1;
	for (start = path;; start = end + 1) {
		end = strchr(start, QSTAR_PATH_LIST_SEPARATOR);
		len = end ? (size_t)(end - start) : strlen(start);
		if (len < sizeof(dir)) {
			memcpy(dir, start, len);
			dir[len] = '\0';
			if (!*dir)
				snprintf(dir, sizeof(dir), ".");
			if (qstar_path_join(dir, tool, candidate, sizeof(candidate)) == 0 &&
			    cache_executable_candidate(candidate))
				return snprintf(dst, dstlen, "%s", candidate) < (int)dstlen ?
				    0 : -1;
#if defined(_WIN32)
			if (snprintf(candidate, sizeof(candidate), "%s/%s.exe", dir, tool) <
			    (int)sizeof(candidate) && cache_executable_candidate(candidate))
				return snprintf(dst, dstlen, "%s", candidate) < (int)dstlen ?
				    0 : -1;
#endif
		}
		if (!end)
			break;
	}
	return -1;
}

static int
cache_string_list_has(const struct qstar_string_list *list, const char *value)
{
	size_t i;

	for (i = 0; list && i < list->len; i++) {
		if (strcmp(list->items[i], value) == 0)
			return 1;
	}
	return 0;
}

static const char *
cache_basename(const char *path)
{
	const char *slash, *backslash;

	slash = strrchr(path ? path : "", '/');
	backslash = strrchr(path ? path : "", '\\');
	if (!slash || (backslash && backslash > slash))
		slash = backslash;
	return slash ? slash + 1 : (path ? path : "");
}

static int
cache_contains_folded(const char *s, const char *needle)
{
	size_t i, j, n;

	if (!s || !needle)
		return 0;
	n = strlen(needle);
	for (i = 0; s[i]; i++) {
		for (j = 0; j < n && s[i + j]; j++) {
			char a = s[i + j], b = needle[j];
			if ('A' <= a && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if ('A' <= b && b <= 'Z')
				b = (char)(b - 'A' + 'a');
			if (a != b)
				break;
		}
		if (j == n)
			return 1;
	}
	return 0;
}

static int
cache_external_interaction(const struct qstar_action_cache_spec *spec,
    char *reason, size_t reason_len)
{
	const char *arg, *tool;
	size_t i;

	tool = spec->argv && spec->argv[0] ? cache_basename(spec->argv[0]) : "";
	if (cache_contains_folded(tool, "qemu-system") ||
	    cache_contains_folded(tool, "hil")) {
		snprintf(reason, reason_len, "external-runtime-tool");
		return 1;
	}
	for (i = 0; spec->argv && spec->argv[i]; i++) {
		arg = spec->argv[i];
		if (strncmp(arg, "/dev/", 5) == 0 || strstr(arg, "/dev/tty") ||
		    strncmp(arg, "/Volumes/", 9) == 0 ||
		    cache_contains_folded(arg, "serial") ||
		    cache_contains_folded(arg, "hardware-in-loop")) {
			snprintf(reason, reason_len, "external-device-or-volume");
			return 1;
		}
	}
	return 0;
}

static void
cache_report_undeclared_path(const struct qstar_action_cache_spec *spec,
    struct qstar_action_cache_decision *decision)
{
	const char *arg;
	size_t i;

	snprintf(decision->audit, sizeof(decision->audit),
	    "declared_inputs=%zu declared_outputs=%zu undeclared-access=unobserved",
	    spec->inputs ? spec->inputs->len : 0,
	    spec->outputs ? spec->outputs->len : 0);
	for (i = 1; spec->argv && spec->argv[i]; i++) {
		arg = spec->argv[i];
		if ((i > 0 && strcmp(spec->argv[i - 1], "-MF") == 0) ||
		    arg[0] == '-' || !strchr(arg, '/') ||
		    cache_string_list_has(spec->inputs, arg) ||
		    cache_string_list_has(spec->outputs, arg))
			continue;
		snprintf(decision->audit, sizeof(decision->audit),
		    "declared_inputs=%zu declared_outputs=%zu undeclared-path=%.*s report-only",
		    spec->inputs ? spec->inputs->len : 0,
		    spec->outputs ? spec->outputs->len : 0, 64, arg);
		break;
	}
}

int
qstar_action_cache_mode(const struct qstar_graph *graph,
    const struct qstar_build_options *options)
{
	if (options && options->action_cache_mode != QSTAR_ACTION_CACHE_INHERIT)
		return options->action_cache_mode;
	if (graph && graph->project.action_cache &&
	    strcmp(graph->project.action_cache, "local") == 0)
		return QSTAR_ACTION_CACHE_LOCAL;
	return QSTAR_ACTION_CACHE_OFF;
}

int
qstar_action_cache_evaluate(struct qstar_graph *graph,
    const struct qstar_action_cache_spec *spec,
    struct qstar_action_cache_decision *decision)
{
	const char *env_keys[] = {"PATH", "SDKROOT", "CPATH", "LIBRARY_PATH", NULL};
	char tool_path[QSTAR_PATH_MAX], input_path[QSTAR_PATH_MAX], digest[32];
	struct qstar_string_list effective_inputs;
	unsigned long long h, base_h, file_h, size;
	unsigned int mode;
	size_t i;

	memset(decision, 0, sizeof(*decision));
	if (!spec) {
		snprintf(decision->reason, sizeof(decision->reason), "missing-action-spec");
		return 0;
	}
	cache_report_undeclared_path(spec, decision);
	if (!spec->declared_cacheable) {
		snprintf(decision->reason, sizeof(decision->reason), "declared-non-cacheable");
		return 0;
	}
	if (!spec->kind || (strcmp(spec->kind, "compile") != 0 &&
	    strcmp(spec->kind, "generate") != 0)) {
		snprintf(decision->reason, sizeof(decision->reason), "action-kind-not-supported");
		return 0;
	}
	if (!spec->outputs || spec->outputs->len == 0) {
		snprintf(decision->reason, sizeof(decision->reason), "no-owned-outputs");
		return 0;
	}
	if (cache_external_interaction(spec, decision->reason,
	    sizeof(decision->reason)))
		return 0;
	if (!spec->argv || !spec->argv[0] ||
	    cache_resolve_tool(graph, spec->argv[0], tool_path, sizeof(tool_path)) < 0 ||
	    cache_file_hash(tool_path, &file_h, &size, &mode) < 0) {
		snprintf(decision->reason, sizeof(decision->reason), "tool-unresolved");
		return 0;
	}
	h = cache_hash_string(QSTAR_CAS_HASH_INIT, tool_path);
	h = cache_hash_bytes(h, &file_h, sizeof(file_h));
	h = cache_hash_bytes(h, &size, sizeof(size));
	cache_format_hash(h, decision->tool_fingerprint,
	    sizeof(decision->tool_fingerprint));
	h = QSTAR_CAS_HASH_INIT;
	for (i = 0; env_keys[i]; i++) {
		h = cache_hash_string(h, env_keys[i]);
		h = cache_hash_string(h, getenv(env_keys[i]));
	}
	for (i = 0; spec->env && i < spec->env->len; i++)
		h = cache_hash_string(h, spec->env->items[i]);
	cache_format_hash(h, decision->env_fingerprint,
	    sizeof(decision->env_fingerprint));
	memset(&effective_inputs, 0, sizeof(effective_inputs));
	for (i = 0; spec->inputs && i < spec->inputs->len; i++) {
		if (!cache_string_list_has(&effective_inputs, spec->inputs->items[i]) &&
		    qstar_string_list_push(&effective_inputs, spec->inputs->items[i]) < 0) {
			qstar_string_list_free(&effective_inputs);
			return qstar_set_error(graph, "qstar: out of memory");
		}
	}
	if (spec->depfile && *spec->depfile &&
	    qstar_parse_depfile_inputs(graph, spec->depfile, &effective_inputs,
	    NULL) < 0) {
		qstar_string_list_free(&effective_inputs);
		return -1;
	}
	h = QSTAR_CAS_HASH_INIT;
	h = cache_hash_string(h, QSTAR_CAS_MAGIC);
	h = cache_hash_string(h, spec->id);
	h = cache_hash_string(h, spec->kind);
	for (i = 0; spec->argv && spec->argv[i]; i++)
		h = cache_hash_string(h, spec->argv[i]);
	h = cache_hash_string(h, decision->tool_fingerprint);
	h = cache_hash_string(h, decision->env_fingerprint);
	base_h = h;
	for (i = 0; spec->inputs && i < spec->inputs->len; i++) {
		base_h = cache_hash_string(base_h, spec->inputs->items[i]);
		if (cache_full_path(graph, spec->inputs->items[i], input_path,
		    sizeof(input_path)) < 0 ||
		    cache_file_hash(input_path, &file_h, &size, &mode) < 0) {
			base_h = cache_hash_string(base_h, "<missing>");
			continue;
		}
		cache_format_hash(file_h, digest, sizeof(digest));
		base_h = cache_hash_string(base_h, digest);
		base_h = cache_hash_bytes(base_h, &size, sizeof(size));
	}
	for (i = 0; spec->outputs && i < spec->outputs->len; i++)
		base_h = cache_hash_string(base_h, spec->outputs->items[i]);
	cache_format_hash(base_h, decision->base_key,
	    sizeof(decision->base_key));
	for (i = 0; i < effective_inputs.len; i++) {
		h = cache_hash_string(h, effective_inputs.items[i]);
		if (cache_full_path(graph, effective_inputs.items[i], input_path,
		    sizeof(input_path)) < 0 ||
		    cache_file_hash(input_path, &file_h, &size, &mode) < 0) {
			h = cache_hash_string(h, "<missing>");
			continue;
		}
		cache_format_hash(file_h, digest, sizeof(digest));
		h = cache_hash_string(h, digest);
		h = cache_hash_bytes(h, &size, sizeof(size));
	}
	for (i = 0; spec->outputs && i < spec->outputs->len; i++)
		h = cache_hash_string(h, spec->outputs->items[i]);
	qstar_string_list_free(&effective_inputs);
	cache_format_hash(h, decision->key, sizeof(decision->key));
	decision->cacheable = 1;
	snprintf(decision->reason, sizeof(decision->reason), "eligible");
	return 0;
}

static int
cache_entry_paths(const struct qstar_graph *graph, const char *key, char *dir,
    size_t dirlen, char *manifest, size_t manifestlen)
{
	char sub[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX];

	if (snprintf(sub, sizeof(sub), "cas/v1/%s", key) >= (int)sizeof(sub) ||
	    qstar_graph_build_path(graph, sub, rel, sizeof(rel)) < 0 ||
	    cache_full_path(graph, rel, dir, dirlen) < 0 ||
	    qstar_path_join(dir, "manifest", manifest, manifestlen) < 0)
		return -1;
	return 0;
}

static int
cache_alias_path(const struct qstar_graph *graph, const char *base_key, char *dst,
    size_t dstlen)
{
	char sub[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX];

	if (snprintf(sub, sizeof(sub), "cas/v1/index/%s", base_key) >=
	    (int)sizeof(sub) || qstar_graph_build_path(graph, sub, rel,
	    sizeof(rel)) < 0 || cache_full_path(graph, rel, dst, dstlen) < 0)
		return -1;
	return 0;
}

static int
cache_read_alias(const struct qstar_graph *graph, const char *base_key, char *key,
    size_t key_len)
{
	char path[QSTAR_PATH_MAX];
	FILE *f;
	size_t i, n;

	if (cache_alias_path(graph, base_key, path, sizeof(path)) < 0 ||
	    !(f = fopen(path, "rb")))
		return 0;
	if (!fgets(key, (int)key_len, f)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	n = strlen(key);
	while (n && (key[n - 1] == '\n' || key[n - 1] == '\r'))
		key[--n] = '\0';
	if (n != 16)
		return 0;
	for (i = 0; i < n; i++) {
		if (!(('0' <= key[i] && key[i] <= '9') ||
		    ('a' <= key[i] && key[i] <= 'f')))
			return 0;
	}
	return 1;
}

static int
cache_write_alias(struct qstar_graph *graph, const char *base_key,
    const char *key)
{
	char path[QSTAR_PATH_MAX], tmp[QSTAR_PATH_MAX];
	FILE *f;

	if (cache_alias_path(graph, base_key, path, sizeof(path)) < 0 ||
	    cache_mkdir_parent(path) < 0 || snprintf(tmp, sizeof(tmp), "%s.tmp",
	    path) >= (int)sizeof(tmp) || !(f = fopen(tmp, "wb")))
		return qstar_set_error(graph,
		    "qstar: could not write local action cache alias");
	fprintf(f, "%s\n", key);
	if (fclose(f) != 0 || (remove(path), rename(tmp, path)) < 0) {
		remove(tmp);
		return qstar_set_error(graph,
		    "qstar: could not commit local action cache alias");
	}
	return 0;
}

static void
cache_discard_entry(const char *dir, size_t output_count)
{
	char path[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < output_count; i++) {
		if (snprintf(path, sizeof(path), "%s/blob.%zu", dir, i) <
		    (int)sizeof(path))
			remove(path);
	}
	if (snprintf(path, sizeof(path), "%s/manifest", dir) < (int)sizeof(path))
		remove(path);
	if (snprintf(path, sizeof(path), "%s/blob.depfile", dir) < (int)sizeof(path))
		remove(path);
	remove(dir);
}

static int
cache_parse_manifest(FILE *f, const struct qstar_action_cache_spec *spec,
    const struct qstar_action_cache_decision *decision, unsigned long long *digests,
    unsigned int *modes, char *depfile, size_t depfile_len,
    unsigned long long *depfile_digest, unsigned int *depfile_mode)
{
	char line[QSTAR_PATH_MAX * 2], expected[QSTAR_PATH_MAX + 32];
	size_t i, count;

	if (!fgets(line, sizeof(line), f) ||
	    strcmp(line, QSTAR_CAS_MAGIC "\n") != 0 ||
	    !fgets(line, sizeof(line), f) ||
	    snprintf(expected, sizeof(expected), "key=%s\n", decision->key) >=
	    (int)sizeof(expected) || strcmp(line, expected) != 0 ||
	    !fgets(line, sizeof(line), f) || sscanf(line, "count=%zu", &count) != 1 ||
	    count != spec->outputs->len || !fgets(line, sizeof(line), f) ||
	    strncmp(line, "depfile=", 8) != 0)
		return -1;
	snprintf(depfile, depfile_len, "%s", line + 8);
	depfile[strcspn(depfile, "\r\n")] = '\0';
	if (!fgets(line, sizeof(line), f) ||
	    sscanf(line, "depfile_digest=%llx", depfile_digest) != 1 ||
	    !fgets(line, sizeof(line), f) ||
	    sscanf(line, "depfile_mode=%o", depfile_mode) != 1)
		return -1;
	for (i = 0; i < count; i++) {
		if (!fgets(line, sizeof(line), f) ||
		    snprintf(expected, sizeof(expected), "output=%s\n",
		    spec->outputs->items[i]) >= (int)sizeof(expected) ||
		    strcmp(line, expected) != 0 || !fgets(line, sizeof(line), f) ||
		    sscanf(line, "digest=%llx", &digests[i]) != 1 ||
		    !fgets(line, sizeof(line), f) ||
		    sscanf(line, "mode=%o", &modes[i]) != 1)
			return -1;
	}
	return 0;
}

int
qstar_action_cache_restore(struct qstar_graph *graph,
    const struct qstar_action_cache_spec *spec,
    struct qstar_action_cache_decision *decision,
    struct qstar_action_cache_stats *stats, char *reason, size_t reason_len)
{
	char dir[QSTAR_PATH_MAX], manifest[QSTAR_PATH_MAX], blob[QSTAR_PATH_MAX];
	char output[QSTAR_PATH_MAX], selected_key[32], stored_depfile[QSTAR_PATH_MAX];
	unsigned long long *digests, digest, size, depfile_digest;
	unsigned int *modes, mode, depfile_mode;
	struct qstar_action_cache_decision stored_decision, refreshed;
	FILE *f;
	size_t i;
	int alias_used, corrupt;

	if (!decision->cacheable) {
		snprintf(reason, reason_len, "%s", decision->reason);
		return 0;
	}
	snprintf(selected_key, sizeof(selected_key), "%s", decision->key);
	alias_used = 0;
	if (cache_entry_paths(graph, selected_key, dir, sizeof(dir), manifest,
	    sizeof(manifest)) < 0)
		return qstar_set_error(graph, "qstar: local action cache path too long");
	f = fopen(manifest, "rb");
	if (!f) {
		if (cache_read_alias(graph, decision->base_key, selected_key,
		    sizeof(selected_key)) == 1 && cache_entry_paths(graph, selected_key,
		    dir, sizeof(dir), manifest, sizeof(manifest)) == 0)
			f = fopen(manifest, "rb"), alias_used = f != NULL;
	}
	if (!f) {
		if (stats)
			stats->misses++;
		snprintf(reason, reason_len, "entry-missing");
		return 0;
	}
	digests = calloc(spec->outputs->len, sizeof(digests[0]));
	modes = calloc(spec->outputs->len, sizeof(modes[0]));
	if (!digests || !modes) {
		free(digests);
		free(modes);
		fclose(f);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	stored_decision = *decision;
	snprintf(stored_decision.key, sizeof(stored_decision.key), "%s",
	    selected_key);
	stored_depfile[0] = '\0';
	depfile_digest = 0;
	depfile_mode = 0;
	corrupt = cache_parse_manifest(f, spec, &stored_decision, digests, modes,
	    stored_depfile, sizeof(stored_depfile), &depfile_digest,
	    &depfile_mode) < 0;
	fclose(f);
	if (!corrupt && stored_depfile[0]) {
		if (!spec->depfile || strcmp(stored_depfile, spec->depfile) != 0 ||
		    snprintf(blob, sizeof(blob), "%s/blob.depfile", dir) >=
		    (int)sizeof(blob) || cache_file_hash(blob, &digest, &size, &mode) < 0 ||
		    digest != depfile_digest)
			corrupt = 1;
	}
	for (i = 0; !corrupt && i < spec->outputs->len; i++) {
		if (snprintf(blob, sizeof(blob), "%s/blob.%zu", dir, i) >=
		    (int)sizeof(blob) || cache_file_hash(blob, &digest, &size, &mode) < 0 ||
		    digest != digests[i])
			corrupt = 1;
	}
	if (corrupt) {
		cache_discard_entry(dir, spec->outputs->len);
		if (stats) {
			stats->misses++;
			stats->corruptions++;
		}
		snprintf(reason, reason_len, "corrupt-entry");
		free(digests);
		free(modes);
		return 0;
	}
	if (alias_used && stored_depfile[0]) {
		if (cache_full_path(graph, stored_depfile, output, sizeof(output)) < 0 ||
		    snprintf(blob, sizeof(blob), "%s/blob.depfile", dir) >=
		    (int)sizeof(blob) || cache_copy_file(blob, output, depfile_mode) < 0) {
			free(digests);
			free(modes);
			return qstar_set_error(graph,
			    "qstar: could not materialize local cache depfile '%s'",
			    stored_depfile);
		}
		if (qstar_action_cache_evaluate(graph, spec, &refreshed) < 0) {
			free(digests);
			free(modes);
			return -1;
		}
		if (strcmp(refreshed.key, selected_key) != 0) {
			if (stats)
				stats->misses++;
			snprintf(reason, reason_len, "dependency-changed");
			free(digests);
			free(modes);
			return 0;
		}
	}
	for (i = 0; i < spec->outputs->len; i++) {
		snprintf(blob, sizeof(blob), "%s/blob.%zu", dir, i);
		if (cache_full_path(graph, spec->outputs->items[i], output,
		    sizeof(output)) < 0 || cache_copy_file(blob, output, modes[i]) < 0) {
			free(digests);
			free(modes);
			return qstar_set_error(graph,
			    "qstar: could not materialize local cache output '%s'",
			    spec->outputs->items[i]);
		}
	}
	free(digests);
	free(modes);
	if (stats)
		stats->hits++;
	snprintf(decision->key, sizeof(decision->key), "%s", selected_key);
	snprintf(reason, reason_len, "hit");
	return 1;
}

int
qstar_action_cache_store(struct qstar_graph *graph,
    const struct qstar_action_cache_spec *spec,
    const struct qstar_action_cache_decision *decision,
    struct qstar_action_cache_stats *stats, char *reason, size_t reason_len)
{
	char dir[QSTAR_PATH_MAX], manifest[QSTAR_PATH_MAX], manifest_tmp[QSTAR_PATH_MAX];
	char output[QSTAR_PATH_MAX], blob[QSTAR_PATH_MAX], blob_tmp[QSTAR_PATH_MAX];
	char stored_depfile[QSTAR_PATH_MAX];
	unsigned long long *digests, digest, size, depfile_digest;
	unsigned int *modes, blob_mode, depfile_mode;
	struct stat st;
	FILE *f;
	size_t i;

	if (!decision->cacheable) {
		snprintf(reason, reason_len, "%s", decision->reason);
		return 0;
	}
	if (cache_entry_paths(graph, decision->key, dir, sizeof(dir), manifest,
	    sizeof(manifest)) < 0 || cache_mkdir_p(dir) < 0)
		return qstar_set_error(graph, "qstar: could not create local action cache entry");
	digests = calloc(spec->outputs->len, sizeof(digests[0]));
	modes = calloc(spec->outputs->len, sizeof(modes[0]));
	if (!digests || !modes) {
		free(digests);
		free(modes);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	f = fopen(manifest, "rb");
	if (f) {
		int valid;

		stored_depfile[0] = '\0';
		depfile_digest = 0;
		depfile_mode = 0;
		valid = cache_parse_manifest(f, spec, decision, digests, modes,
		    stored_depfile, sizeof(stored_depfile), &depfile_digest,
		    &depfile_mode) == 0;
		fclose(f);
		if (valid && stored_depfile[0]) {
			if (snprintf(blob, sizeof(blob), "%s/blob.depfile", dir) >=
			    (int)sizeof(blob) || cache_file_hash(blob, &digest, &size,
			    &blob_mode) < 0 || digest != depfile_digest)
				valid = 0;
		}
		for (i = 0; valid && i < spec->outputs->len; i++) {
			if (snprintf(blob, sizeof(blob), "%s/blob.%zu", dir, i) >=
			    (int)sizeof(blob) || cache_file_hash(blob, &digest, &size,
			    &blob_mode) < 0 || digest != digests[i])
				valid = 0;
		}
		if (valid) {
			if (cache_write_alias(graph, decision->base_key,
			    decision->key) < 0) {
				free(digests);
				free(modes);
				return -1;
			}
			free(digests);
			free(modes);
			snprintf(reason, reason_len, "already-present");
			return 0;
		}
		if (stats)
			stats->corruptions++;
		cache_discard_entry(dir, spec->outputs->len);
		if (cache_mkdir_p(dir) < 0) {
			free(digests);
			free(modes);
			return qstar_set_error(graph,
			    "qstar: could not recreate local action cache entry");
		}
	}
	stored_depfile[0] = '\0';
	depfile_digest = 0;
	depfile_mode = 0;
	if (spec->depfile && *spec->depfile) {
		if (cache_full_path(graph, spec->depfile, output, sizeof(output)) < 0 ||
		    stat(output, &st) < 0 || !S_ISREG(st.st_mode) ||
		    cache_file_hash(output, &depfile_digest, &size,
		    &depfile_mode) < 0) {
			free(digests);
			free(modes);
			snprintf(reason, reason_len, "depfile-missing");
			return 0;
		}
		snprintf(stored_depfile, sizeof(stored_depfile), "%s", spec->depfile);
		if (snprintf(blob, sizeof(blob), "%s/blob.depfile", dir) >=
		    (int)sizeof(blob) || snprintf(blob_tmp, sizeof(blob_tmp), "%s.tmp",
		    blob) >= (int)sizeof(blob_tmp) ||
		    cache_copy_file(output, blob_tmp, depfile_mode) < 0 ||
		    (remove(blob), rename(blob_tmp, blob)) < 0) {
			remove(blob_tmp);
			free(digests);
			free(modes);
			return qstar_set_error(graph,
			    "qstar: could not store local cache depfile");
		}
	}
	for (i = 0; i < spec->outputs->len; i++) {
		if (cache_full_path(graph, spec->outputs->items[i], output,
		    sizeof(output)) < 0 || stat(output, &st) < 0 || !S_ISREG(st.st_mode) ||
		    cache_file_hash(output, &digests[i], &size, &modes[i]) < 0) {
			free(digests);
			free(modes);
			snprintf(reason, reason_len, "non-file-output");
			return 0;
		}
		if (snprintf(blob, sizeof(blob), "%s/blob.%zu", dir, i) >=
		    (int)sizeof(blob) || snprintf(blob_tmp, sizeof(blob_tmp), "%s.tmp",
		    blob) >= (int)sizeof(blob_tmp) ||
		    cache_copy_file(output, blob_tmp, modes[i]) < 0 ||
		    (remove(blob), rename(blob_tmp, blob)) < 0) {
			remove(blob_tmp);
			free(digests);
			free(modes);
			return qstar_set_error(graph, "qstar: could not store local cache blob");
		}
	}
	if (snprintf(manifest_tmp, sizeof(manifest_tmp), "%s.tmp", manifest) >=
	    (int)sizeof(manifest_tmp) || !(f = fopen(manifest_tmp, "wb"))) {
		free(digests);
		free(modes);
		return qstar_set_error(graph, "qstar: could not write local cache manifest");
	}
	fprintf(f,
	    QSTAR_CAS_MAGIC "\nkey=%s\ncount=%zu\ndepfile=%s\ndepfile_digest=%016llx\ndepfile_mode=%o\n",
	    decision->key, spec->outputs->len, stored_depfile, depfile_digest,
	    depfile_mode);
	for (i = 0; i < spec->outputs->len; i++)
		fprintf(f, "output=%s\ndigest=%016llx\nmode=%o\n",
		    spec->outputs->items[i], digests[i], modes[i]);
	if (fclose(f) != 0 || (remove(manifest), rename(manifest_tmp, manifest)) < 0) {
		remove(manifest_tmp);
		free(digests);
		free(modes);
		return qstar_set_error(graph, "qstar: could not commit local cache manifest");
	}
	if (cache_write_alias(graph, decision->base_key, decision->key) < 0) {
		free(digests);
		free(modes);
		return -1;
	}
	free(digests);
	free(modes);
	if (stats)
		stats->stores++;
	snprintf(reason, reason_len, "stored");
	return 1;
}

void
qstar_action_cache_print_audit(FILE *out,
    const struct qstar_action_cache_spec *spec,
    const struct qstar_action_cache_decision *decision)
{
	fprintf(out,
	    "hermeticity_audit id=%s kind=%s enforcement=report-only cacheable=%s reason=%s tool=%s env=%s %s\n",
	    spec->id, spec->kind, decision->cacheable ? "true" : "false",
	    decision->reason, decision->tool_fingerprint[0] ?
	    decision->tool_fingerprint : "<none>", decision->env_fingerprint[0] ?
	    decision->env_fingerprint : "<none>", decision->audit);
}

void
qstar_action_cache_print_stats(FILE *out, const char *backend, int mode,
    const struct qstar_action_cache_stats *stats)
{
	fprintf(out,
	    "local_cache_stats backend=%s mode=%s audited=%zu eligible=%zu non_cacheable=%zu hits=%zu misses=%zu stores=%zu corruptions=%zu\n",
	    backend, mode == QSTAR_ACTION_CACHE_LOCAL ? "local" : "off",
	    stats->audited, stats->eligible, stats->non_cacheable, stats->hits,
	    stats->misses, stats->stores, stats->corruptions);
}
