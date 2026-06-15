#include "internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define QSTAR_STELLA_CACHE_SCHEMA "qstar-stella-plan-cache-v5"
#define QSTAR_STELLA_GRAPH_MAGIC "qstar-stella-graph-cache-v1"
#define QSTAR_STELLA_ACTION_MAGIC "qstar-stella-actions-cache-v1"
#define QSTAR_STELLA_PLAN_ABI 6
#define QSTAR_STELLA_HASH_INIT 1469598103934665603ULL
#define QSTAR_STELLA_HASH_PRIME 1099511628211ULL
#define QSTAR_STELLA_MAX_STRING (16U * 1024U * 1024U)

struct cache_file_fp {
	char *path;
	unsigned long long size;
	unsigned long long mtime_ns;
	unsigned long long hash;
};

static void
set_reason(char *reason, size_t reason_len, const char *value)
{
	if (reason && reason_len)
		snprintf(reason, reason_len, "%s", value ? value : "unknown");
}

static const char *
string_or_empty(const char *s)
{
	return s ? s : "";
}

static const char *
label_key(const char *label)
{
	return label && *label ? label : "<all>";
}

static unsigned long long
hash_update(unsigned long long h, const void *data, size_t len)
{
	const unsigned char *p;
	size_t i;

	p = data;
	for (i = 0; i < len; i++) {
		h ^= p[i];
		h *= QSTAR_STELLA_HASH_PRIME;
	}
	return h;
}

static unsigned long long
hash_string(unsigned long long h, const char *s)
{
	if (!s)
		s = "";
	h = hash_update(h, s, strlen(s));
	return hash_update(h, "\n", 1);
}

static int
full_path(const struct qstar_graph *graph, const char *rel, char *dst, size_t dstlen)
{
	const char *root;
	int n;

	root = graph->package_root && *graph->package_root ? graph->package_root : ".";
	if (strcmp(root, ".") == 0)
		n = snprintf(dst, dstlen, "%s", rel);
	else
		n = snprintf(dst, dstlen, "%s/%s", root, rel);
	return n >= 0 && (size_t)n < dstlen ? 0 : -1;
}

static int
mkdir_p(const char *path)
{
	char tmp[QSTAR_PATH_MAX];
	char *p;

	if (!path || !*path)
		return -1;
	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
		return -1;
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (qstar_platform_mkdir(tmp, 0777) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (qstar_platform_mkdir(tmp, 0777) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int
mkdir_parent(const char *path)
{
	char dir[QSTAR_PATH_MAX];

	if (qstar_dirname(path, dir, sizeof(dir)) < 0)
		return -1;
	return mkdir_p(dir);
}

static int
cache_rel(const struct qstar_graph *graph, const char *name, char *dst, size_t dstlen)
{
	char sub[QSTAR_PATH_MAX];

	if (snprintf(sub, sizeof(sub), "stella/%s", name) >= (int)sizeof(sub))
		return -1;
	return qstar_graph_build_path(graph, sub, dst, dstlen);
}

static int
cache_full(const struct qstar_graph *graph, const char *name, char *dst, size_t dstlen)
{
	char rel[QSTAR_PATH_MAX];

	if (cache_rel(graph, name, rel, sizeof(rel)) < 0)
		return -1;
	return full_path(graph, rel, dst, dstlen);
}

static int
cache_tmp_full(const struct qstar_graph *graph, const char *name, char *dst, size_t dstlen)
{
	char sub[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX];

	if (snprintf(sub, sizeof(sub), "stella/tmp/%s.%ld.tmp", name,
	    (long)getpid()) >= (int)sizeof(sub))
		return -1;
	if (qstar_graph_build_path(graph, sub, rel, sizeof(rel)) < 0)
		return -1;
	return full_path(graph, rel, dst, dstlen);
}

static int
write_u64(FILE *f, unsigned long long v)
{
	uint64_t x;

	x = (uint64_t)v;
	return fwrite(&x, sizeof(x), 1, f) == 1 ? 0 : -1;
}

static int
read_u64(FILE *f, unsigned long long *v)
{
	uint64_t x;

	if (fread(&x, sizeof(x), 1, f) != 1)
		return -1;
	*v = (unsigned long long)x;
	return 0;
}

static int
write_i32(FILE *f, int v)
{
	int32_t x;

	x = (int32_t)v;
	return fwrite(&x, sizeof(x), 1, f) == 1 ? 0 : -1;
}

static int
read_i32(FILE *f, int *v)
{
	int32_t x;

	if (fread(&x, sizeof(x), 1, f) != 1)
		return -1;
	*v = (int)x;
	return 0;
}

static int
write_str(FILE *f, const char *s)
{
	size_t len;

	if (!s)
		return write_u64(f, UINT64_MAX);
	len = strlen(s);
	if (write_u64(f, (unsigned long long)len) < 0)
		return -1;
	return len == 0 || fwrite(s, 1, len, f) == len ? 0 : -1;
}

static int
read_str(FILE *f, char **out)
{
	unsigned long long len64;
	size_t len;
	char *s;

	if (read_u64(f, &len64) < 0)
		return -1;
	if (len64 == UINT64_MAX) {
		*out = NULL;
		return 0;
	}
	if (len64 > QSTAR_STELLA_MAX_STRING)
		return -1;
	len = (size_t)len64;
	s = malloc(len + 1);
	if (!s)
		return -1;
	if (len > 0 && fread(s, 1, len, f) != len) {
		free(s);
		return -1;
	}
	s[len] = '\0';
	*out = s;
	return 0;
}

static int
write_list(FILE *f, const struct qstar_string_list *list)
{
	size_t i;

	if (write_u64(f, (unsigned long long)list->len) < 0)
		return -1;
	for (i = 0; i < list->len; i++) {
		if (write_str(f, list->items[i]) < 0)
			return -1;
	}
	return 0;
}

static int
read_list(FILE *f, struct qstar_string_list *list)
{
	unsigned long long len64;
	size_t i;
	char *s;

	if (read_u64(f, &len64) < 0 || len64 > 1000000ULL)
		return -1;
	for (i = 0; i < (size_t)len64; i++) {
		if (read_str(f, &s) < 0)
			return -1;
		if (qstar_string_list_push(list, s) < 0) {
			free(s);
			return -1;
		}
		free(s);
	}
	return 0;
}

static int
write_profile(FILE *f, const struct qstar_profile_input *p)
{
#define WSTR(field) do { if (write_str(f, p->field) < 0) return -1; } while (0)
#define WLIST(field) do { if (write_list(f, &p->field) < 0) return -1; } while (0)
	WSTR(name);
	WSTR(target);
	WSTR(toolchain);
	WSTR(stdlib_policy);
	WSTR(cc);
	WSTR(cxx);
	WSTR(ar);
	WSTR(linker);
	WSTR(sysroot);
	WSTR(resource_dir);
	WSTR(response_files);
	WSTR(response_style);
	WSTR(allow_absolute_tools);
	WLIST(artifact_names);
	WLIST(compile_options);
	WLIST(include_dirs);
	WLIST(lib_dirs);
	WLIST(link_options);
	WLIST(path_tools);
	WLIST(tool_overrides);
#undef WSTR
#undef WLIST
	return 0;
}

static int
read_profile(FILE *f, struct qstar_profile_input *p)
{
#define RSTR(field) do { if (read_str(f, &p->field) < 0) return -1; } while (0)
#define RLIST(field) do { if (read_list(f, &p->field) < 0) return -1; } while (0)
	RSTR(name);
	RSTR(target);
	RSTR(toolchain);
	RSTR(stdlib_policy);
	RSTR(cc);
	RSTR(cxx);
	RSTR(ar);
	RSTR(linker);
	RSTR(sysroot);
	RSTR(resource_dir);
	RSTR(response_files);
	RSTR(response_style);
	RSTR(allow_absolute_tools);
	RLIST(artifact_names);
	RLIST(compile_options);
	RLIST(include_dirs);
	RLIST(lib_dirs);
	RLIST(link_options);
	RLIST(path_tools);
	RLIST(tool_overrides);
#undef RSTR
#undef RLIST
	return 0;
}

static int
write_toolset(FILE *f, const struct qstar_toolset *toolset)
{
	return write_str(f, toolset->label) < 0 ||
	    write_str(f, toolset->name) < 0 ||
	    write_str(f, toolset->fragment_dir) < 0 ||
	    write_str(f, toolset->origin_file) < 0 ||
	    write_i32(f, toolset->origin_line) < 0 ||
	    write_list(f, &toolset->c) < 0 ||
	    write_list(f, &toolset->cxx) < 0 ||
	    write_list(f, &toolset->asm_) < 0 ||
	    write_list(f, &toolset->archive) < 0 ||
	    write_list(f, &toolset->link) < 0 ||
	    write_list(f, &toolset->path_tools) < 0 ||
	    write_str(f, toolset->response_files) < 0 ||
	    write_str(f, toolset->response_style) < 0 ||
	    write_str(f, toolset->allow_absolute_tools) < 0 ? -1 : 0;
}

static int
read_toolset(FILE *f, struct qstar_toolset *toolset)
{
	return read_str(f, &toolset->label) < 0 ||
	    read_str(f, &toolset->name) < 0 ||
	    read_str(f, &toolset->fragment_dir) < 0 ||
	    read_str(f, &toolset->origin_file) < 0 ||
	    read_i32(f, &toolset->origin_line) < 0 ||
	    read_list(f, &toolset->c) < 0 ||
	    read_list(f, &toolset->cxx) < 0 ||
	    read_list(f, &toolset->asm_) < 0 ||
	    read_list(f, &toolset->archive) < 0 ||
	    read_list(f, &toolset->link) < 0 ||
	    read_list(f, &toolset->path_tools) < 0 ||
	    read_str(f, &toolset->response_files) < 0 ||
	    read_str(f, &toolset->response_style) < 0 ||
	    read_str(f, &toolset->allow_absolute_tools) < 0 ? -1 : 0;
}

static int
write_target(FILE *f, const struct qstar_target *t)
{
#define WSTR(field) do { if (write_str(f, t->field) < 0) return -1; } while (0)
#define WLIST(field) do { if (write_list(f, &t->field) < 0) return -1; } while (0)
	WSTR(label);
	WSTR(name);
	WSTR(kind);
	WSTR(fragment_dir);
	WSTR(origin_file);
	if (write_i32(f, t->origin_line) < 0 || write_i32(f, t->modules.present) < 0)
		return -1;
	if (write_str(f, t->modules.root) < 0 ||
	    write_list(f, &t->modules.include) < 0 ||
	    write_list(f, &t->modules.exclude) < 0)
		return -1;
	WLIST(configs);
	WLIST(sources);
	WLIST(public_headers);
	WLIST(private_headers);
	WLIST(include_dirs);
	WLIST(public_include_dirs);
	WLIST(private_include_dirs);
	WLIST(interface_include_dirs);
	WLIST(system_include_dirs);
	WLIST(deps);
	WLIST(private_deps);
	WLIST(visibility);
	WLIST(libs);
	WLIST(lib_dirs);
	WLIST(frameworks);
	WLIST(link_options);
	WLIST(link_inputs);
	WLIST(cflags);
	WLIST(cxxflags);
	WLIST(asm_include_dirs);
	WLIST(asm_compile_options);
	WLIST(run_command);
	WSTR(description);
	WSTR(artifact_name);
	WSTR(cxx_standard);
	WSTR(run_marker);
	WSTR(run_marker_log);
	if (write_i32(f, t->run_timeout_sec) < 0 ||
	    write_i32(f, t->asm_preprocess) < 0 ||
	    write_i32(f, t->cxx_modules_present) < 0 ||
	    write_i32(f, t->cxx_modules_enabled) < 0)
		return -1;
	WSTR(toolset);
	WSTR(toolchain);
	WSTR(stdlib_policy);
#undef WSTR
#undef WLIST
	return 0;
}

static int
read_target(FILE *f, struct qstar_target *t)
{
#define RSTR(field) do { if (read_str(f, &t->field) < 0) return -1; } while (0)
#define RLIST(field) do { if (read_list(f, &t->field) < 0) return -1; } while (0)
	RSTR(label);
	RSTR(name);
	RSTR(kind);
	RSTR(fragment_dir);
	RSTR(origin_file);
	if (read_i32(f, &t->origin_line) < 0 ||
	    read_i32(f, &t->modules.present) < 0)
		return -1;
	if (read_str(f, &t->modules.root) < 0 ||
	    read_list(f, &t->modules.include) < 0 ||
	    read_list(f, &t->modules.exclude) < 0)
		return -1;
	RLIST(configs);
	RLIST(sources);
	RLIST(public_headers);
	RLIST(private_headers);
	RLIST(include_dirs);
	RLIST(public_include_dirs);
	RLIST(private_include_dirs);
	RLIST(interface_include_dirs);
	RLIST(system_include_dirs);
	RLIST(deps);
	RLIST(private_deps);
	RLIST(visibility);
	RLIST(libs);
	RLIST(lib_dirs);
	RLIST(frameworks);
	RLIST(link_options);
	RLIST(link_inputs);
	RLIST(cflags);
	RLIST(cxxflags);
	RLIST(asm_include_dirs);
	RLIST(asm_compile_options);
	RLIST(run_command);
	RSTR(description);
	RSTR(artifact_name);
	RSTR(cxx_standard);
	RSTR(run_marker);
	RSTR(run_marker_log);
	if (read_i32(f, &t->run_timeout_sec) < 0 ||
	    read_i32(f, &t->asm_preprocess) < 0 ||
	    read_i32(f, &t->cxx_modules_present) < 0 ||
	    read_i32(f, &t->cxx_modules_enabled) < 0)
		return -1;
	RSTR(toolset);
	RSTR(toolchain);
	RSTR(stdlib_policy);
#undef RSTR
#undef RLIST
	return 0;
}

static int
write_genrule(FILE *f, const struct qstar_genrule *g)
{
	if (write_str(f, g->label) < 0 ||
	    write_str(f, g->name) < 0 ||
	    write_str(f, g->fragment_dir) < 0 ||
	    write_str(f, g->origin_file) < 0 ||
	    write_i32(f, g->origin_line) < 0 ||
	    write_str(f, g->tool) < 0 ||
	    write_str(f, g->description) < 0 ||
	    write_i32(f, g->config_header) < 0 ||
	    write_list(f, &g->inputs) < 0 ||
	    write_list(f, &g->outputs) < 0 ||
	    write_list(f, &g->output_groups) < 0 ||
	    write_list(f, &g->output_formats) < 0 ||
	    write_list(f, &g->output_addresses) < 0 ||
	    write_list(f, &g->output_layouts) < 0 ||
	    write_list(f, &g->args) < 0 ||
	    write_list(f, &g->command) < 0)
		return -1;
	return 0;
}

static int
read_genrule(FILE *f, struct qstar_genrule *g)
{
	if (read_str(f, &g->label) < 0 ||
	    read_str(f, &g->name) < 0 ||
	    read_str(f, &g->fragment_dir) < 0 ||
	    read_str(f, &g->origin_file) < 0 ||
	    read_i32(f, &g->origin_line) < 0 ||
	    read_str(f, &g->tool) < 0 ||
	    read_str(f, &g->description) < 0 ||
	    read_i32(f, &g->config_header) < 0 ||
	    read_list(f, &g->inputs) < 0 ||
	    read_list(f, &g->outputs) < 0 ||
	    read_list(f, &g->output_groups) < 0 ||
	    read_list(f, &g->output_formats) < 0 ||
	    read_list(f, &g->output_addresses) < 0 ||
	    read_list(f, &g->output_layouts) < 0 ||
	    read_list(f, &g->args) < 0 ||
	    read_list(f, &g->command) < 0)
		return -1;
	return 0;
}

static int
write_stage(FILE *f, const struct qstar_stage *s)
{
	return write_str(f, s->label) < 0 ||
	    write_str(f, s->name) < 0 ||
	    write_str(f, s->fragment_dir) < 0 ||
	    write_str(f, s->origin_file) < 0 ||
	    write_i32(f, s->origin_line) < 0 ||
	    write_str(f, s->root) < 0 ||
	    write_str(f, s->description) < 0 ||
	    write_list(f, &s->srcs) < 0 ||
	    write_list(f, &s->dsts) < 0 ? -1 : 0;
}

static int
read_stage(FILE *f, struct qstar_stage *s)
{
	return read_str(f, &s->label) < 0 ||
	    read_str(f, &s->name) < 0 ||
	    read_str(f, &s->fragment_dir) < 0 ||
	    read_str(f, &s->origin_file) < 0 ||
	    read_i32(f, &s->origin_line) < 0 ||
	    read_str(f, &s->root) < 0 ||
	    read_str(f, &s->description) < 0 ||
	    read_list(f, &s->srcs) < 0 ||
	    read_list(f, &s->dsts) < 0 ? -1 : 0;
}

static int
write_family(FILE *f, const struct qstar_target_family *family)
{
	return write_str(f, family->name) < 0 ||
	    write_str(f, family->fragment_dir) < 0 ||
	    write_str(f, family->origin_file) < 0 ||
	    write_i32(f, family->origin_line) < 0 ||
	    write_i32(f, family->allow_shared_sources) < 0 ||
	    write_list(f, &family->variants) < 0 ||
	    write_list(f, &family->targets) < 0 ? -1 : 0;
}

static int
read_family(FILE *f, struct qstar_target_family *family)
{
	return read_str(f, &family->name) < 0 ||
	    read_str(f, &family->fragment_dir) < 0 ||
	    read_str(f, &family->origin_file) < 0 ||
	    read_i32(f, &family->origin_line) < 0 ||
	    read_i32(f, &family->allow_shared_sources) < 0 ||
	    read_list(f, &family->variants) < 0 ||
	    read_list(f, &family->targets) < 0 ? -1 : 0;
}

static int
write_graph_cache_file(struct qstar_graph *graph, const char *path)
{
	FILE *f;
	size_t i;

	f = fopen(path, "wb");
	if (!f)
		return -1;
	if (write_str(f, QSTAR_STELLA_GRAPH_MAGIC) < 0 ||
	    write_u64(f, QSTAR_STELLA_PLAN_ABI) < 0 ||
	    write_str(f, graph->package_root) < 0 ||
	    write_str(f, graph->generator) < 0 ||
	    write_str(f, graph->requested_generator) < 0 ||
	    write_str(f, graph->build_dir_override) < 0 ||
	    write_i32(f, graph->project.present) < 0 ||
	    write_str(f, graph->project.name) < 0 ||
	    write_str(f, graph->project.version) < 0 ||
	    write_str(f, graph->project.root) < 0 ||
	    write_str(f, graph->project.build_dir) < 0 ||
	    write_str(f, graph->project.generated_dir) < 0 ||
	    write_str(f, graph->project.compile_commands) < 0 ||
	    write_i32(f, graph->uses_file_globs) < 0 ||
	    write_profile(f, &graph->profile) < 0 ||
	    write_list(f, &graph->evaluated_fragments) < 0 ||
	    write_u64(f, (unsigned long long)graph->package_len) < 0) {
		fclose(f);
		return -1;
	}
	for (i = 0; i < graph->package_len; i++) {
		if (write_str(f, graph->packages[i].alias) < 0 ||
		    write_str(f, graph->packages[i].root) < 0) {
			fclose(f);
			return -1;
		}
	}
	if (write_u64(f, (unsigned long long)graph->toolset_len) < 0) {
		fclose(f);
		return -1;
	}
	for (i = 0; i < graph->toolset_len; i++) {
		if (write_toolset(f, &graph->toolsets[i]) < 0) {
			fclose(f);
			return -1;
		}
	}
	if (write_u64(f, (unsigned long long)graph->len) < 0) {
		fclose(f);
		return -1;
	}
	for (i = 0; i < graph->len; i++) {
		if (write_target(f, &graph->targets[i]) < 0) {
			fclose(f);
			return -1;
		}
	}
	if (write_u64(f, (unsigned long long)graph->genrule_len) < 0) {
		fclose(f);
		return -1;
	}
	for (i = 0; i < graph->genrule_len; i++) {
		if (write_genrule(f, &graph->genrules[i]) < 0) {
			fclose(f);
			return -1;
		}
	}
	if (write_u64(f, (unsigned long long)graph->stage_len) < 0) {
		fclose(f);
		return -1;
	}
	for (i = 0; i < graph->stage_len; i++) {
		if (write_stage(f, &graph->stages[i]) < 0) {
			fclose(f);
			return -1;
		}
	}
	if (write_u64(f, (unsigned long long)graph->family_len) < 0) {
		fclose(f);
		return -1;
	}
	for (i = 0; i < graph->family_len; i++) {
		if (write_family(f, &graph->families[i]) < 0) {
			fclose(f);
			return -1;
		}
	}
	if (fclose(f) != 0)
		return -1;
	return 0;
}

static int
read_graph_cache_file(const char *path, struct qstar_graph *out)
{
	FILE *f;
	char *magic;
	unsigned long long version, n, i;

	qstar_graph_init(out);
	f = fopen(path, "rb");
	if (!f)
		return -1;
	if (read_str(f, &magic) < 0) {
		fclose(f);
		return -1;
	}
	if (strcmp(magic, QSTAR_STELLA_GRAPH_MAGIC) != 0) {
		free(magic);
		fclose(f);
		return -1;
	}
	free(magic);
	if (read_u64(f, &version) < 0 || version != QSTAR_STELLA_PLAN_ABI ||
	    read_str(f, &out->package_root) < 0 ||
	    read_str(f, &out->generator) < 0 ||
	    read_str(f, &out->requested_generator) < 0 ||
	    read_str(f, &out->build_dir_override) < 0 ||
	    read_i32(f, &out->project.present) < 0 ||
	    read_str(f, &out->project.name) < 0 ||
	    read_str(f, &out->project.version) < 0 ||
	    read_str(f, &out->project.root) < 0 ||
	    read_str(f, &out->project.build_dir) < 0 ||
	    read_str(f, &out->project.generated_dir) < 0 ||
	    read_str(f, &out->project.compile_commands) < 0 ||
	    read_i32(f, &out->uses_file_globs) < 0 ||
	    read_profile(f, &out->profile) < 0 ||
	    read_list(f, &out->evaluated_fragments) < 0 ||
	    read_u64(f, &n) < 0 || n > 1000000ULL) {
		fclose(f);
		qstar_graph_free(out);
		return -1;
	}
	out->packages = calloc((size_t)n ? (size_t)n : 1, sizeof(out->packages[0]));
	if (!out->packages) {
		fclose(f);
		qstar_graph_free(out);
		return -1;
	}
	out->package_len = out->package_cap = (size_t)n;
	for (i = 0; i < n; i++) {
		if (read_str(f, &out->packages[i].alias) < 0 ||
		    read_str(f, &out->packages[i].root) < 0) {
			fclose(f);
			qstar_graph_free(out);
			return -1;
		}
	}
	if (read_u64(f, &n) < 0 || n > 1000000ULL) {
		fclose(f);
		qstar_graph_free(out);
		return -1;
	}
	out->toolsets = calloc((size_t)n ? (size_t)n : 1, sizeof(out->toolsets[0]));
	if (!out->toolsets) {
		fclose(f);
		qstar_graph_free(out);
		return -1;
	}
	out->toolset_len = out->toolset_cap = (size_t)n;
	for (i = 0; i < n; i++) {
		if (read_toolset(f, &out->toolsets[i]) < 0) {
			fclose(f);
			qstar_graph_free(out);
			return -1;
		}
	}
	if (read_u64(f, &n) < 0 || n > 1000000ULL) {
		fclose(f);
		qstar_graph_free(out);
		return -1;
	}
	out->targets = calloc((size_t)n ? (size_t)n : 1, sizeof(out->targets[0]));
	if (!out->targets) {
		fclose(f);
		qstar_graph_free(out);
		return -1;
	}
	out->len = out->cap = (size_t)n;
	for (i = 0; i < n; i++) {
		if (read_target(f, &out->targets[i]) < 0) {
			fclose(f);
			qstar_graph_free(out);
			return -1;
		}
	}
	if (read_u64(f, &n) < 0 || n > 1000000ULL) {
		fclose(f);
		qstar_graph_free(out);
		return -1;
	}
	out->genrules = calloc((size_t)n ? (size_t)n : 1, sizeof(out->genrules[0]));
	if (!out->genrules) {
		fclose(f);
		qstar_graph_free(out);
		return -1;
	}
	out->genrule_len = out->genrule_cap = (size_t)n;
	for (i = 0; i < n; i++) {
		if (read_genrule(f, &out->genrules[i]) < 0) {
			fclose(f);
			qstar_graph_free(out);
			return -1;
		}
	}
	if (read_u64(f, &n) < 0 || n > 1000000ULL) {
		fclose(f);
		qstar_graph_free(out);
		return -1;
	}
	out->stages = calloc((size_t)n ? (size_t)n : 1, sizeof(out->stages[0]));
	if (!out->stages) {
		fclose(f);
		qstar_graph_free(out);
		return -1;
	}
	out->stage_len = out->stage_cap = (size_t)n;
	for (i = 0; i < n; i++) {
		if (read_stage(f, &out->stages[i]) < 0) {
			fclose(f);
			qstar_graph_free(out);
			return -1;
		}
	}
	if (read_u64(f, &n) < 0 || n > 1000000ULL) {
		fclose(f);
		qstar_graph_free(out);
		return -1;
	}
	out->families = calloc((size_t)n ? (size_t)n : 1, sizeof(out->families[0]));
	if (!out->families) {
		fclose(f);
		qstar_graph_free(out);
		return -1;
	}
	out->family_len = out->family_cap = (size_t)n;
	for (i = 0; i < n; i++) {
		if (read_family(f, &out->families[i]) < 0) {
			fclose(f);
			qstar_graph_free(out);
			return -1;
		}
	}
	if (fclose(f) != 0) {
		qstar_graph_free(out);
		return -1;
	}
	return 0;
}

static void
json_string(FILE *f, const char *s)
{
	const unsigned char *p;

	p = (const unsigned char *)string_or_empty(s);
	fputc('"', f);
	while (*p) {
		if (*p == '"' || *p == '\\')
			fprintf(f, "\\%c", *p);
		else if (*p == '\n')
			fputs("\\n", f);
		else if (*p == '\r')
			fputs("\\r", f);
		else if (*p == '\t')
			fputs("\\t", f);
		else if (*p < 0x20)
			fprintf(f, "\\u%04x", *p);
		else
			fputc(*p, f);
		p++;
	}
	fputc('"', f);
}

static char *
read_text_file(const char *path)
{
	FILE *f;
	long n;
	char *buf;

	f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)n + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[n] = '\0';
	fclose(f);
	return buf;
}

static char *
json_get_string(const char *text, const char *key)
{
	char needle[128];
	const char *p, *q;
	char *out;
	size_t cap, len;

	if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle))
		return NULL;
	p = strstr(text, needle);
	if (!p)
		return NULL;
	p = strchr(p + strlen(needle), ':');
	if (!p)
		return NULL;
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != '"')
		return NULL;
	p++;
	cap = strlen(p) + 1;
	out = malloc(cap);
	if (!out)
		return NULL;
	len = 0;
	for (q = p; *q && *q != '"'; q++) {
		if (*q == '\\' && q[1]) {
			q++;
			if (*q == 'n')
				out[len++] = '\n';
			else if (*q == 'r')
				out[len++] = '\r';
			else if (*q == 't')
				out[len++] = '\t';
			else
				out[len++] = *q;
		} else {
			out[len++] = *q;
		}
	}
	out[len] = '\0';
	return out;
}

static int
json_get_ull(const char *text, const char *key, unsigned long long *out)
{
	char needle[128];
	const char *p;

	if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle))
		return -1;
	p = strstr(text, needle);
	if (!p)
		return -1;
	p = strchr(p + strlen(needle), ':');
	if (!p)
		return -1;
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	return sscanf(p, "%llu", out) == 1 ? 0 : -1;
}

static int
json_string_equals(const char *text, const char *key, const char *expected)
{
	char *value;
	int eq;

	value = json_get_string(text, key);
	if (!value)
		return 0;
	eq = strcmp(value, string_or_empty(expected)) == 0;
	free(value);
	return eq;
}

static int
file_hash(const char *path, unsigned long long *hash)
{
	FILE *f;
	unsigned char buf[8192];
	size_t n;
	unsigned long long h;

	f = fopen(path, "rb");
	if (!f)
		return -1;
	h = QSTAR_STELLA_HASH_INIT;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		h = hash_update(h, buf, n);
	if (ferror(f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	*hash = h;
	return 0;
}

static unsigned long long
stat_mtime_ns(const struct stat *st)
{
	return (unsigned long long)st->st_mtime * 1000000000ULL;
}

static int
fingerprint_file(const struct qstar_graph *graph, const char *rel, struct cache_file_fp *fp)
{
	char full[QSTAR_PATH_MAX];
	struct stat st;

	memset(fp, 0, sizeof(*fp));
	if (full_path(graph, rel, full, sizeof(full)) < 0 || stat(full, &st) < 0)
		return -1;
	fp->path = qstar_strdup(rel);
	if (!fp->path)
		return -1;
	fp->size = (unsigned long long)st.st_size;
	fp->mtime_ns = stat_mtime_ns(&st);
	if (file_hash(full, &fp->hash) < 0) {
		free(fp->path);
		memset(fp, 0, sizeof(*fp));
		return -1;
	}
	return 0;
}

static void
fingerprint_free(struct cache_file_fp *fp)
{
	free(fp->path);
	memset(fp, 0, sizeof(*fp));
}

static unsigned long long
input_fingerprint_hash(const struct cache_file_fp *fp, unsigned long long h)
{
	char num[64];

	h = hash_string(h, fp->path);
	snprintf(num, sizeof(num), "%llu", fp->size);
	h = hash_string(h, num);
	snprintf(num, sizeof(num), "%llu", fp->mtime_ns);
	h = hash_string(h, num);
	snprintf(num, sizeof(num), "%016llx", fp->hash);
	return hash_string(h, num);
}

static int
write_inputs_json(struct qstar_graph *graph, const char *path,
    unsigned long long *fingerprint)
{
	FILE *f;
	struct cache_file_fp fp;
	size_t i;
	unsigned long long h;

	f = fopen(path, "wb");
	if (!f)
		return -1;
	h = QSTAR_STELLA_HASH_INIT;
	fputs("[\n", f);
	for (i = 0; i < graph->evaluated_fragments.len; i++) {
		if (fingerprint_file(graph, graph->evaluated_fragments.items[i], &fp) < 0) {
			fclose(f);
			return -1;
		}
		h = input_fingerprint_hash(&fp, h);
		fputs("  {\"path\":", f);
		json_string(f, fp.path);
		fprintf(f, ",\"size\":%llu,\"mtime_ns\":%llu,\"hash\":\"%016llx\"}%s\n",
		    fp.size, fp.mtime_ns, fp.hash,
		    i + 1 == graph->evaluated_fragments.len ? "" : ",");
		fingerprint_free(&fp);
	}
	fputs("]\n", f);
	if (fclose(f) != 0)
		return -1;
	*fingerprint = h;
	return 0;
}

static int
parse_input_object(const char *line, struct cache_file_fp *fp)
{
	char *path, *hash_s;

	memset(fp, 0, sizeof(*fp));
	path = json_get_string(line, "path");
	hash_s = json_get_string(line, "hash");
	if (!path || !hash_s ||
	    json_get_ull(line, "size", &fp->size) < 0 ||
	    json_get_ull(line, "mtime_ns", &fp->mtime_ns) < 0 ||
	    sscanf(hash_s, "%llx", &fp->hash) != 1) {
		free(path);
		free(hash_s);
		return -1;
	}
	fp->path = path;
	free(hash_s);
	return 0;
}

static int
inputs_match(struct qstar_graph *graph, const char *path,
    unsigned long long *fingerprint, char *reason, size_t reason_len)
{
	FILE *f;
	char line[8192];
	struct cache_file_fp cached, current;
	unsigned long long h;
	size_t count;

	f = fopen(path, "rb");
	if (!f) {
		set_reason(reason, reason_len, "inputs-missing");
		return 0;
	}
	h = QSTAR_STELLA_HASH_INIT;
	count = 0;
	while (fgets(line, sizeof(line), f)) {
		if (!strstr(line, "\"path\""))
			continue;
		if (parse_input_object(line, &cached) < 0) {
			fclose(f);
			set_reason(reason, reason_len, "inputs-parse-error");
			return 0;
		}
		if (fingerprint_file(graph, cached.path, &current) < 0) {
			fingerprint_free(&cached);
			fclose(f);
			set_reason(reason, reason_len, "authoring-input-missing");
			return 0;
		}
		if (cached.size != current.size ||
		    cached.mtime_ns != current.mtime_ns ||
		    cached.hash != current.hash) {
			fingerprint_free(&cached);
			fingerprint_free(&current);
			fclose(f);
			set_reason(reason, reason_len, "authoring-input-changed");
			return 0;
		}
		h = input_fingerprint_hash(&current, h);
		fingerprint_free(&cached);
		fingerprint_free(&current);
		count++;
	}
	fclose(f);
	if (count == 0) {
		set_reason(reason, reason_len, "inputs-empty");
		return 0;
	}
	*fingerprint = h;
	return 1;
}

static int
regular_file_exists(const char *path)
{
	FILE *f;

	f = fopen(path, "rb");
	if (!f)
		return 0;
	fclose(f);
	return 1;
}

static int
discover_root_for_cache(const char *file, char *root, size_t rootlen)
{
	char dir[QSTAR_PATH_MAX], cur[QSTAR_PATH_MAX], parent[QSTAR_PATH_MAX];
	char qstar_lua[QSTAR_PATH_MAX];

	if (qstar_dirname(file, dir, sizeof(dir)) < 0)
		return -1;
	snprintf(cur, sizeof(cur), "%s", dir[0] ? dir : ".");
	for (;;) {
		if (qstar_path_join(cur, "qstar.lua", qstar_lua, sizeof(qstar_lua)) == 0 &&
		    regular_file_exists(qstar_lua))
			return snprintf(root, rootlen, "%s", cur) < (int)rootlen ? 0 : -1;
		if (strcmp(cur, ".") == 0 || strcmp(cur, "/") == 0)
			break;
		if (qstar_dirname(cur, parent, sizeof(parent)) < 0 ||
		    strcmp(parent, cur) == 0)
			break;
		snprintf(cur, sizeof(cur), "%s", parent);
	}
	return -1;
}

static unsigned long long
package_alias_hash(const struct qstar_graph *graph)
{
	unsigned long long h;
	size_t i;

	h = QSTAR_STELLA_HASH_INIT;
	for (i = 0; i < graph->package_len; i++) {
		h = hash_string(h, graph->packages[i].alias);
		h = hash_string(h, graph->packages[i].root);
	}
	return h;
}

/** lowered action 하나를 actions.qsa에 기록한다. */
static int
write_cached_action(FILE *f, const struct qstar_cached_action *action)
{
	return write_str(f, action->id) < 0 ||
	    write_str(f, action->kind) < 0 ||
	    write_str(f, action->target_label) < 0 ||
	    write_str(f, action->description) < 0 ||
	    write_str(f, action->depfile) < 0 ||
	    write_str(f, action->source_path) < 0 ||
	    write_u64(f, (unsigned long long)action->source_index) < 0 ||
	    write_i32(f, action->wants_depfile) < 0 ||
	    write_list(f, &action->argv) < 0 ||
	    write_list(f, &action->outputs) < 0 ||
	    write_list(f, &action->inputs) < 0 ||
	    write_list(f, &action->depfile_inputs) < 0 ? -1 : 0;
}

/** actions.qsa에서 lowered action 하나를 읽는다. */
static int
read_cached_action(FILE *f, struct qstar_cached_action *action)
{
	unsigned long long source_index;

	if (read_str(f, &action->id) < 0 ||
	    read_str(f, &action->kind) < 0 ||
	    read_str(f, &action->target_label) < 0 ||
	    read_str(f, &action->description) < 0 ||
	    read_str(f, &action->depfile) < 0 ||
	    read_str(f, &action->source_path) < 0 ||
	    read_u64(f, &source_index) < 0 ||
	    read_i32(f, &action->wants_depfile) < 0 ||
	    read_list(f, &action->argv) < 0 ||
	    read_list(f, &action->outputs) < 0 ||
	    read_list(f, &action->inputs) < 0 ||
	    read_list(f, &action->depfile_inputs) < 0)
		return -1;
	action->source_index = (size_t)source_index;
	action->wants_depfile = action->wants_depfile ? 1 : 0;
	return 0;
}

/** lowered action cache entry가 scheduler에서 복원 가능한 최소 모양인지 확인한다. */
static int
cached_action_shape_ok(const struct qstar_cached_action *action)
{
	return action->id && *action->id &&
	    action->kind && *action->kind &&
	    action->target_label && *action->target_label &&
	    action->argv.len > 0 &&
	    action->outputs.len > 0;
}

static int
write_actions_file(struct qstar_graph *graph, const char *label, const char *path,
    size_t *action_count)
{
	FILE *f;
	size_t i;

	f = fopen(path, "wb");
	if (!f)
		return -1;
	fprintf(f, "%s\n", QSTAR_STELLA_ACTION_MAGIC);
	fprintf(f, "{\"root\":");
	json_string(f, label_key(label));
	fputs("}\n", f);
	if (write_u64(f, (unsigned long long)graph->cached_action_len) < 0) {
		fclose(f);
		return -1;
	}
	for (i = 0; i < graph->cached_action_len; i++) {
		if (write_cached_action(f, &graph->cached_actions[i]) < 0) {
			fclose(f);
			return -1;
		}
	}
	if (fclose(f) != 0)
		return -1;
	*action_count = graph->cached_action_len;
	return 0;
}

/** actions.qsa lowered action plan을 Graph에 복원한다. */
static int
read_actions_file(const char *path, struct qstar_graph *graph, const char *label)
{
	FILE *f;
	struct qstar_cached_action *action;
	char line[QSTAR_PATH_MAX + 128];
	char *root;
	unsigned long long count, i;
	int ok;

	f = fopen(path, "rb");
	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	if (strncmp(line, QSTAR_STELLA_ACTION_MAGIC,
	    strlen(QSTAR_STELLA_ACTION_MAGIC)) != 0) {
		fclose(f);
		return -1;
	}
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	root = json_get_string(line, "root");
	ok = root && strcmp(root, label_key(label)) == 0;
	free(root);
	if (!ok || read_u64(f, &count) < 0 || count > 1000000ULL) {
		fclose(f);
		return -1;
	}
	qstar_graph_clear_cached_actions(graph);
	for (i = 0; i < count; i++) {
		action = qstar_graph_add_cached_action(graph);
		if (!action || read_cached_action(f, action) < 0 ||
		    !cached_action_shape_ok(action)) {
			fclose(f);
			qstar_graph_clear_cached_actions(graph);
			return -1;
		}
	}
	if (fclose(f) != 0) {
		qstar_graph_clear_cached_actions(graph);
		return -1;
	}
	graph->cached_action_plan_loaded = 1;
	return 0;
}

static int
write_manifest(const struct qstar_graph *graph, const char *path, const char *file,
    const char *cmd, const char *label, const char *cli_profile, const char *cli_target,
    const char *cli_toolchain, const char *cli_stdlib, unsigned long long input_hash,
    size_t action_count)
{
	FILE *f;

	f = fopen(path, "wb");
	if (!f)
		return -1;
	fputs("{\n", f);
	fputs("  \"schema\":", f);
	json_string(f, QSTAR_STELLA_CACHE_SCHEMA);
	fputs(",\n  \"qstar_version\":", f);
	json_string(f, QSTAR_VERSION);
	fprintf(f, ",\n  \"plan_abi\":%d", QSTAR_STELLA_PLAN_ABI);
	fputs(",\n  \"package_root\":", f);
	json_string(f, graph->package_root);
	fputs(",\n  \"build_dir\":", f);
	json_string(f, qstar_graph_build_dir(graph));
	fputs(",\n  \"generated_dir\":", f);
	json_string(f, qstar_graph_generated_dir(graph));
	fputs(",\n  \"generator\":", f);
	json_string(f, qstar_graph_generator(graph));
	fputs(",\n  \"requested_generator\":", f);
	json_string(f, qstar_graph_requested_generator(graph));
	fputs(",\n  \"command\":", f);
	json_string(f, cmd);
	fputs(",\n  \"label\":", f);
	json_string(f, label_key(label));
	fputs(",\n  \"entry_file\":", f);
	json_string(f, file);
	fputs(",\n  \"cli_profile\":", f);
	json_string(f, cli_profile);
	fputs(",\n  \"cli_target\":", f);
	json_string(f, cli_target);
	fputs(",\n  \"cli_toolchain\":", f);
	json_string(f, cli_toolchain);
	fputs(",\n  \"cli_stdlib\":", f);
	json_string(f, cli_stdlib);
	fprintf(f, ",\n  \"package_alias_hash\":\"%016llx\"",
	    package_alias_hash(graph));
	fprintf(f, ",\n  \"input_fingerprint\":\"%016llx\"", input_hash);
	fprintf(f, ",\n  \"action_count\":%zu", action_count);
	fprintf(f, ",\n  \"target_count\":%zu", graph->len);
	fprintf(f, ",\n  \"generated_action_count\":%zu\n", graph->genrule_len);
	fputs("}\n", f);
	return fclose(f) == 0 ? 0 : -1;
}

static int
same_package_alias_hash(const char *manifest, const struct qstar_graph *graph)
{
	char expected[32], *actual;
	int ok;

	snprintf(expected, sizeof(expected), "%016llx", package_alias_hash(graph));
	actual = json_get_string(manifest, "package_alias_hash");
	ok = actual && strcmp(actual, expected) == 0;
	free(actual);
	return ok;
}

static int
manifest_matches(const char *manifest, const struct qstar_graph *graph, const char *file,
    const char *cmd, const char *label, const char *cli_profile, const char *cli_target,
    const char *cli_toolchain, const char *cli_stdlib, char *reason, size_t reason_len)
{
	unsigned long long abi;

	if (!json_string_equals(manifest, "schema", QSTAR_STELLA_CACHE_SCHEMA)) {
		set_reason(reason, reason_len, "schema-mismatch");
		return 0;
	}
	if (!json_string_equals(manifest, "qstar_version", QSTAR_VERSION)) {
		set_reason(reason, reason_len, "version-mismatch");
		return 0;
	}
	if (json_get_ull(manifest, "plan_abi", &abi) < 0 ||
	    abi != QSTAR_STELLA_PLAN_ABI) {
		set_reason(reason, reason_len, "plan-abi-mismatch");
		return 0;
	}
	if (!json_string_equals(manifest, "package_root", graph->package_root)) {
		set_reason(reason, reason_len, "package-root-mismatch");
		return 0;
	}
	if (!json_string_equals(manifest, "build_dir", qstar_graph_build_dir(graph))) {
		set_reason(reason, reason_len, "build-dir-mismatch");
		return 0;
	}
	if (!json_string_equals(manifest, "generator", qstar_graph_generator(graph))) {
		set_reason(reason, reason_len, "generator-mismatch");
		return 0;
	}
	if (!json_string_equals(manifest, "command", cmd) ||
	    !json_string_equals(manifest, "label", label_key(label)) ||
	    !json_string_equals(manifest, "entry_file", file)) {
		set_reason(reason, reason_len, "request-mismatch");
		return 0;
	}
	if (!json_string_equals(manifest, "cli_profile", string_or_empty(cli_profile)) ||
	    !json_string_equals(manifest, "cli_target", string_or_empty(cli_target)) ||
	    !json_string_equals(manifest, "cli_toolchain", string_or_empty(cli_toolchain)) ||
	    !json_string_equals(manifest, "cli_stdlib", string_or_empty(cli_stdlib))) {
		set_reason(reason, reason_len, "profile-input-mismatch");
		return 0;
	}
	if (!same_package_alias_hash(manifest, graph)) {
		set_reason(reason, reason_len, "package-alias-mismatch");
		return 0;
	}
	return 1;
}

static int
rename_atomic(const char *tmp, const char *dst)
{
	if (rename(tmp, dst) == 0)
		return 0;
	unlink(tmp);
	return -1;
}

int
qstar_stella_plan_cache_try_load(struct qstar_graph *graph, const char *file,
    const char *cmd, const char *label, const char *cli_profile, const char *cli_target,
    const char *cli_toolchain, const char *cli_stdlib, char *reason, size_t reason_len)
{
	struct qstar_graph loaded;
	char root[QSTAR_PATH_MAX], manifest_path[QSTAR_PATH_MAX], inputs_path[QSTAR_PATH_MAX];
	char graph_path[QSTAR_PATH_MAX], actions_path[QSTAR_PATH_MAX];
	char *manifest;
	unsigned long long input_hash;
	int ok;

	set_reason(reason, reason_len, "miss");
	if (strcmp(cmd, "build") != 0) {
		set_reason(reason, reason_len, "command-not-build");
		return 0;
	}
	if (strcmp(qstar_graph_generator(graph), "stella") != 0) {
		set_reason(reason, reason_len, "generator-not-stella");
		return 0;
	}
	if (discover_root_for_cache(file, root, sizeof(root)) < 0 ||
	    qstar_graph_set_package_root(graph, root) < 0) {
		set_reason(reason, reason_len, "root-discovery-failed");
		return 0;
	}
	if (cache_full(graph, "manifest.json", manifest_path, sizeof(manifest_path)) < 0 ||
	    cache_full(graph, "inputs.json", inputs_path, sizeof(inputs_path)) < 0 ||
	    cache_full(graph, "graph.qsg", graph_path, sizeof(graph_path)) < 0 ||
	    cache_full(graph, "actions.qsa", actions_path, sizeof(actions_path)) < 0) {
		set_reason(reason, reason_len, "cache-path-too-long");
		return 0;
	}
	manifest = read_text_file(manifest_path);
	if (!manifest) {
		set_reason(reason, reason_len, "manifest-missing");
		return 0;
	}
	ok = manifest_matches(manifest, graph, file, cmd, label, cli_profile, cli_target,
	    cli_toolchain, cli_stdlib, reason, reason_len);
	free(manifest);
	if (!ok)
		return 0;
	if (access(actions_path, R_OK) < 0) {
		set_reason(reason, reason_len, "action-plan-missing");
		return 0;
	}
	if (!inputs_match(graph, inputs_path, &input_hash, reason, reason_len))
		return 0;
	(void)input_hash;
	if (read_graph_cache_file(graph_path, &loaded) < 0) {
		set_reason(reason, reason_len, "graph-cache-parse-error");
		return 0;
	}
	if (read_actions_file(actions_path, &loaded, label) < 0) {
		qstar_graph_free(&loaded);
		set_reason(reason, reason_len, "action-plan-parse-error");
		return 0;
	}
	qstar_graph_free(graph);
	*graph = loaded;
	set_reason(reason, reason_len, "hit");
	return 1;
}

int
qstar_stella_plan_cache_store(struct qstar_graph *graph, const char *file,
    const char *cmd, const char *label, const char *cli_profile, const char *cli_target,
    const char *cli_toolchain, const char *cli_stdlib, char *reason, size_t reason_len)
{
	char cache_dir[QSTAR_PATH_MAX], tmp_dir[QSTAR_PATH_MAX];
	char manifest_path[QSTAR_PATH_MAX], manifest_tmp[QSTAR_PATH_MAX];
	char inputs_path[QSTAR_PATH_MAX], inputs_tmp[QSTAR_PATH_MAX];
	char graph_path[QSTAR_PATH_MAX], graph_tmp[QSTAR_PATH_MAX];
	char actions_path[QSTAR_PATH_MAX], actions_tmp[QSTAR_PATH_MAX];
	unsigned long long input_hash;
	size_t action_count;

	if (strcmp(cmd, "build") != 0 || strcmp(qstar_graph_generator(graph), "stella") != 0) {
		set_reason(reason, reason_len, "not-cacheable-command");
		return 0;
	}
	if (graph->uses_file_globs) {
		set_reason(reason, reason_len, "file-glob-not-cacheable");
		return 0;
	}
	if (cache_full(graph, "", cache_dir, sizeof(cache_dir)) < 0 ||
	    cache_full(graph, "tmp", tmp_dir, sizeof(tmp_dir)) < 0 ||
	    cache_full(graph, "manifest.json", manifest_path, sizeof(manifest_path)) < 0 ||
	    cache_tmp_full(graph, "manifest.json", manifest_tmp, sizeof(manifest_tmp)) < 0 ||
	    cache_full(graph, "inputs.json", inputs_path, sizeof(inputs_path)) < 0 ||
	    cache_tmp_full(graph, "inputs.json", inputs_tmp, sizeof(inputs_tmp)) < 0 ||
	    cache_full(graph, "graph.qsg", graph_path, sizeof(graph_path)) < 0 ||
	    cache_tmp_full(graph, "graph.qsg", graph_tmp, sizeof(graph_tmp)) < 0 ||
	    cache_full(graph, "actions.qsa", actions_path, sizeof(actions_path)) < 0 ||
	    cache_tmp_full(graph, "actions.qsa", actions_tmp, sizeof(actions_tmp)) < 0) {
		set_reason(reason, reason_len, "cache-path-too-long");
		return -1;
	}
	if (mkdir_p(cache_dir) < 0 || mkdir_p(tmp_dir) < 0) {
		set_reason(reason, reason_len, "cache-dir-create-failed");
		return -1;
	}
	if (qstar_graph_prepare_lowered_action_cache(graph, label) < 0) {
		set_reason(reason, reason_len, "action-plan-prepare-failed");
		return -1;
	}
	(void)mkdir_parent(graph_tmp);
	if (write_graph_cache_file(graph, graph_tmp) < 0 ||
	    rename_atomic(graph_tmp, graph_path) < 0) {
		set_reason(reason, reason_len, "graph-cache-write-failed");
		return -1;
	}
	if (write_actions_file(graph, label, actions_tmp, &action_count) < 0 ||
	    rename_atomic(actions_tmp, actions_path) < 0) {
		set_reason(reason, reason_len, "action-plan-write-failed");
		return -1;
	}
	if (write_inputs_json(graph, inputs_tmp, &input_hash) < 0 ||
	    rename_atomic(inputs_tmp, inputs_path) < 0) {
		set_reason(reason, reason_len, "inputs-write-failed");
		return -1;
	}
	if (write_manifest(graph, manifest_tmp, file, cmd, label, cli_profile, cli_target,
	    cli_toolchain, cli_stdlib, input_hash, action_count) < 0 ||
	    rename_atomic(manifest_tmp, manifest_path) < 0) {
		set_reason(reason, reason_len, "manifest-write-failed");
		return -1;
	}
	set_reason(reason, reason_len, "stored");
	return 1;
}
