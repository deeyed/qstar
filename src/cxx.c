#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define qstar_mkdir(path) _mkdir(path)
#else
#include <unistd.h>
#define qstar_mkdir(path) mkdir(path, 0777)
#endif

static const char *
path_basename(const char *path)
{
	const char *slash, *backslash;

	slash = strrchr(path ? path : "", '/');
	backslash = strrchr(path ? path : "", '\\');
	if (!slash || (backslash && backslash > slash))
		slash = backslash;
	return slash ? slash + 1 : (path ? path : "");
}

const char *
qstar_cxx_compiler_family(const struct qstar_resolved_toolchain *resolved)
{
	const char *tool;

	tool = path_basename(resolved ? resolved->cxx : "");
#ifdef __APPLE__
	if (strcmp(resolved ? resolved->cxx : "", "/usr/bin/clang++") == 0 ||
	    strcmp(resolved ? resolved->cxx : "", "clang++") == 0)
		return "apple-clang";
#endif
	if (strstr(tool, "clang"))
		return "clang";
	if (strstr(tool, "g++") || strstr(tool, "gcc"))
		return "gcc";
	if (strcmp(tool, "c++") == 0)
#ifdef __APPLE__
		return "apple-clang";
#else
		return "gcc";
#endif
	if (strcmp(tool, "cl") == 0 || strcmp(tool, "cl.exe") == 0)
		return "msvc";
	return "unknown";
}

static int
target_has_cxx_source(const struct qstar_target *target)
{
	struct qstar_source_info source;
	size_t i;

	for (i = 0; target && i < target->sources.len; i++) {
		if (qstar_target_source_classify(target, i, &source) == 0 &&
		    strcmp(source.provider, "cxx") == 0 &&
		    qstar_source_requires_compile(&source))
			return 1;
	}
	return 0;
}

int
qstar_cxx_source_is_module_interface(const struct qstar_target *target,
    size_t source_index)
{
	struct qstar_source_info source;

	return target && source_index < target->sources.len &&
	    qstar_target_source_classify(target, source_index, &source) == 0 &&
	    qstar_source_is_cxx_module(&source);
}

int
qstar_cxx_source_is_implementation(const struct qstar_target *target,
    size_t source_index)
{
	struct qstar_source_info source;

	return target && source_index < target->sources.len &&
	    qstar_target_source_classify(target, source_index, &source) == 0 &&
	    strcmp(source.provider, "cxx") == 0 &&
	    strcmp(source.language, "cxx") == 0 &&
	    !qstar_target_provider_source_unit(target, source_index);
}

int
qstar_cxx_target_has_module_interfaces(const struct qstar_target *target)
{
	size_t i;

	for (i = 0; target && i < target->sources.len; i++) {
		if (qstar_cxx_source_is_module_interface(target, i))
			return 1;
	}
	return 0;
}

static int
cxx_standard_supports_modules(const char *standard)
{
	const char *version;

	if (!standard)
		return 0;
	if (strncmp(standard, "c++", 3) == 0)
		version = standard + 3;
	else if (strncmp(standard, "gnu++", 5) == 0)
		version = standard + 5;
	else
		return 0;
	return strcmp(version, "20") == 0 || strcmp(version, "23") == 0 ||
	    strcmp(version, "26") == 0 || strcmp(version, "2a") == 0 ||
	    strcmp(version, "2b") == 0 || strcmp(version, "2c") == 0;
}

int
qstar_cxx_validate_strategies(struct qstar_graph *graph,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *resolved)
{
	const char *family;
	int enabled;

	enabled = target && ((target->cxx_precompiled_header &&
	    *target->cxx_precompiled_header) || target->cxx_unity_enabled ||
	    target->cxx_modules_enabled);
	if (!enabled)
		return 0;
	if (!target_has_cxx_source(target))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "lang.cxx", target->label,
		    "qstar: target '%s' enables a C++ build strategy but has no built-in C++ source",
		    target->label);
	if (target->compile_context && strcmp(target->kind, "objectlib") == 0 &&
	    strcmp(target->compile_context, "consumer") == 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "lang.cxx", target->label,
		    "qstar: opt-in C++ strategies are not supported on compile_context = \"consumer\" objectlib '%s'; configure the consuming target instead",
		    target->label);
	family = qstar_cxx_compiler_family(resolved);
	if (strcmp(family, "clang") != 0 && strcmp(family, "apple-clang") != 0 &&
	    strcmp(family, "gcc") != 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "lang.cxx", target->label,
		    "qstar: C++ build strategies require a directly configured Clang or GCC compiler; '%s' has unknown capability family",
		    resolved && resolved->cxx[0] ? resolved->cxx : "<none>");
	if (target->cxx_modules_enabled && strcmp(family, "clang") != 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "lang.cxx.modules", target->label,
		    "qstar: lang.cxx.modules requires Clang in this release; compiler '%s' is classified as %s",
		    resolved->cxx, family);
	if (target->cxx_modules_enabled &&
	    !cxx_standard_supports_modules(target->cxx_standard))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "lang.cxx.standard", target->label,
		    "qstar: lang.cxx.modules requires standard = \"c++20\" or newer");
	if (target->cxx_modules_enabled && !qstar_cxx_target_has_module_interfaces(target))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "lang.cxx.modules", target->label,
		    "qstar: lang.cxx.modules.enabled = true requires at least one .cppm or .ixx module interface source");
	if (target->cxx_modules_enabled) {
		size_t i, j;
		char left[QSTAR_PATH_MAX], right[QSTAR_PATH_MAX];

		for (i = 0; i < target->sources.len; i++) {
			if (!qstar_cxx_source_is_module_interface(target, i))
				continue;
			for (j = i + 1; j < target->sources.len; j++) {
				if (!qstar_cxx_source_is_module_interface(target, j))
					continue;
				if (qstar_cxx_module_output_path(graph, target, i, left,
				    sizeof(left)) < 0 || qstar_cxx_module_output_path(graph,
				    target, j, right, sizeof(right)) < 0)
					return qstar_set_error(graph,
					    "qstar: C++ module BMI output path is too long");
				if (strcmp(left, right) == 0)
					return qstar_set_error_origin(graph, target->origin_file,
					    target->origin_line, "sources", target->label,
					    "qstar: C++ module interface sources '%s' and '%s' produce the same BMI name; interface basenames must be unique",
					    target->sources.items[i], target->sources.items[j]);
			}
		}
	}
	return 0;
}

int
qstar_cxx_unity_source_info(const struct qstar_target *target, size_t source_index,
    size_t *batch, int *leader)
{
	size_t i, ordinal, batch_size;

	if (!target || !target->cxx_unity_enabled ||
	    !qstar_cxx_source_is_implementation(target, source_index))
		return 0;
	batch_size = target->cxx_unity_batch_size > 0 ?
	    (size_t)target->cxx_unity_batch_size : 8;
	ordinal = 0;
	for (i = 0; i < target->sources.len; i++) {
		if (!qstar_cxx_source_is_implementation(target, i))
			continue;
		if (i == source_index) {
			if (batch)
				*batch = ordinal / batch_size;
			if (leader)
				*leader = ordinal % batch_size == 0;
			return 1;
		}
		ordinal++;
	}
	return 0;
}

size_t
qstar_cxx_unity_batch_count(const struct qstar_target *target)
{
	size_t i, count, batch_size;

	if (!target || !target->cxx_unity_enabled)
		return 0;
	count = 0;
	for (i = 0; i < target->sources.len; i++) {
		if (qstar_cxx_source_is_implementation(target, i))
			count++;
	}
	batch_size = target->cxx_unity_batch_size > 0 ?
	    (size_t)target->cxx_unity_batch_size : 8;
	return (count + batch_size - 1) / batch_size;
}

int
qstar_cxx_unity_batch_sources(const struct qstar_target *target, size_t batch,
    struct qstar_string_list *sources)
{
	size_t i, ordinal, batch_size, begin, end;

	memset(sources, 0, sizeof(*sources));
	batch_size = target->cxx_unity_batch_size > 0 ?
	    (size_t)target->cxx_unity_batch_size : 8;
	begin = batch * batch_size;
	end = begin + batch_size;
	ordinal = 0;
	for (i = 0; i < target->sources.len; i++) {
		if (!qstar_cxx_source_is_implementation(target, i))
			continue;
		if (ordinal >= begin && ordinal < end &&
		    qstar_string_list_push(sources, target->sources.items[i]) < 0) {
			qstar_string_list_free(sources);
			return -1;
		}
		ordinal++;
	}
	return sources->len ? 0 : -1;
}

static int
cxx_subpath(const struct qstar_graph *graph, const struct qstar_target *target,
    const char *tail, char *dst, size_t dstlen)
{
	char owner[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX];
	int n;

	qstar_mangle_label(target->label, owner, sizeof(owner));
	n = snprintf(sub, sizeof(sub), "out/%s/cxx/%s", owner, tail);
	return n >= 0 && (size_t)n < sizeof(sub) ?
	    qstar_graph_build_path(graph, sub, dst, dstlen) : -1;
}

int
qstar_cxx_pch_output_path(const struct qstar_graph *graph,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *resolved,
    char *dst, size_t dstlen)
{
	return cxx_subpath(graph, target,
	    strcmp(qstar_cxx_compiler_family(resolved), "gcc") == 0 ?
	    "pch.hpp.gch" : "pch.pch", dst, dstlen);
}

int
qstar_cxx_pch_include_path(const struct qstar_graph *graph,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *resolved,
    char *dst, size_t dstlen)
{
	if (qstar_cxx_pch_output_path(graph, target, resolved, dst, dstlen) < 0)
		return -1;
	if (strcmp(qstar_cxx_compiler_family(resolved), "gcc") == 0) {
		size_t len = strlen(dst);
		if (len < 4 || strcmp(dst + len - 4, ".gch") != 0)
			return -1;
		dst[len - 4] = '\0';
	}
	return 0;
}

int
qstar_cxx_module_dir_path(const struct qstar_graph *graph,
    const struct qstar_target *target, char *dst, size_t dstlen)
{
	return cxx_subpath(graph, target, "modules", dst, dstlen);
}

int
qstar_cxx_module_output_path(const struct qstar_graph *graph,
    const struct qstar_target *target, size_t source_index, char *dst, size_t dstlen)
{
	const char *base, *dot;
	char name[256], tail[QSTAR_PATH_MAX];
	size_t len;

	base = path_basename(target->sources.items[source_index]);
	dot = strrchr(base, '.');
	len = dot ? (size_t)(dot - base) : strlen(base);
	if (len == 0 || len >= sizeof(name))
		return -1;
	memcpy(name, base, len);
	name[len] = '\0';
	if (snprintf(tail, sizeof(tail), "modules/%s.pcm", name) >=
	    (int)sizeof(tail))
		return -1;
	return cxx_subpath(graph, target, tail, dst, dstlen);
}

int
qstar_cxx_unity_object_path(const struct qstar_graph *graph,
    const struct qstar_target *target, size_t batch, char *dst, size_t dstlen)
{
	char tail[128];
	snprintf(tail, sizeof(tail), "unity/unity_%zu.o", batch);
	return cxx_subpath(graph, target, tail, dst, dstlen);
}

int
qstar_cxx_unity_depfile_path(const struct qstar_graph *graph,
    const struct qstar_target *target, size_t batch, char *dst, size_t dstlen)
{
	char tail[128];
	snprintf(tail, sizeof(tail), "unity/unity_%zu.d", batch);
	return cxx_subpath(graph, target, tail, dst, dstlen);
}

int
qstar_cxx_unity_source_path(const struct qstar_graph *graph,
    const struct qstar_target *target,
    size_t batch, char *dst, size_t dstlen)
{
	char tail[128];
	snprintf(tail, sizeof(tail), "unity/unity_%zu.cpp", batch);
	return cxx_subpath(graph, target, tail, dst, dstlen);
}

static int
mkdir_parents(char *path)
{
	char *p;

	for (p = path + 1; *p; p++) {
		if (*p != '/' && *p != '\\')
			continue;
		if (p == path + 2 && path[1] == ':')
			continue;
		char saved = *p;
		*p = '\0';
		if (qstar_mkdir(path) < 0 && errno != EEXIST) {
			*p = saved;
			return -1;
		}
		*p = saved;
	}
	return 0;
}

static int
same_file_contents(const char *path, const char *contents, size_t len)
{
	FILE *f;
	char *buf;
	long size;
	int same;

	f = fopen(path, "rb");
	if (!f)
		return 0;
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0 || (size_t)size != len) {
		fclose(f);
		return 0;
	}
	buf = malloc(len ? len : 1);
	if (!buf) {
		fclose(f);
		return 0;
	}
	same = fread(buf, 1, len, f) == len && memcmp(buf, contents, len) == 0;
	free(buf);
	fclose(f);
	return same;
}

int
qstar_cxx_materialize_unity_source(struct qstar_graph *graph,
    const struct qstar_target *target, size_t batch, char *path, size_t pathlen)
{
	struct qstar_string_list sources;
	char full[QSTAR_PATH_MAX], *contents;
	const char *root;
	size_t i, cap, len;
	FILE *f;

	if (qstar_cxx_unity_source_path(graph, target, batch, path, pathlen) < 0 ||
	    qstar_cxx_unity_batch_sources(target, batch, &sources) < 0)
		return qstar_set_error(graph, "qstar: C++ unity source path is invalid");
	cap = 128;
	for (i = 0; i < sources.len; i++)
		cap += strlen(sources.items[i]) * 2 + 64;
	contents = malloc(cap);
	if (!contents) {
		qstar_string_list_free(&sources);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	len = (size_t)snprintf(contents, cap,
	    "// Generated by QStar C++ unity strategy.\n");
	for (i = 0; i < sources.len; i++)
		len += (size_t)snprintf(contents + len, cap - len,
		    "#line 1 \"%s\"\n#include \"%s\"\n", sources.items[i],
		    sources.items[i]);
	root = graph->package_root && *graph->package_root ? graph->package_root : ".";
	if (qstar_path_join(root, path, full, sizeof(full)) < 0) {
		free(contents);
		qstar_string_list_free(&sources);
		return qstar_set_error(graph, "qstar: C++ unity source path is too long");
	}
	if (!same_file_contents(full, contents, len)) {
		if (mkdir_parents(full) < 0 || !(f = fopen(full, "wb"))) {
			free(contents);
			qstar_string_list_free(&sources);
			return qstar_set_error(graph, "qstar: could not write C++ unity source '%s'",
			    path);
		}
		if (fwrite(contents, 1, len, f) != len) {
			fclose(f);
			free(contents);
			qstar_string_list_free(&sources);
			return qstar_set_error(graph, "qstar: could not commit C++ unity source '%s'",
			    path);
		}
		if (fclose(f) != 0) {
			free(contents);
			qstar_string_list_free(&sources);
			return qstar_set_error(graph, "qstar: could not commit C++ unity source '%s'",
			    path);
		}
	}
	free(contents);
	qstar_string_list_free(&sources);
	return 0;
}

int
qstar_cxx_collect_module_inputs(const struct qstar_graph *graph,
    const struct qstar_target *target, size_t source_index,
    struct qstar_string_list *inputs)
{
	size_t i;
	char bmi[QSTAR_PATH_MAX];
	int is_interface;

	memset(inputs, 0, sizeof(*inputs));
	if (!target->cxx_modules_enabled ||
	    (!qstar_cxx_source_is_module_interface(target, source_index) &&
	    !qstar_cxx_source_is_implementation(target, source_index)))
		return 0;
	is_interface = qstar_cxx_source_is_module_interface(target, source_index);
	for (i = 0; i < target->sources.len; i++) {
		if (!qstar_cxx_source_is_module_interface(target, i))
			continue;
		if (is_interface && i >= source_index)
			break;
		if (qstar_cxx_module_output_path(graph, target, i, bmi, sizeof(bmi)) < 0 ||
		    qstar_string_list_push(inputs, bmi) < 0) {
			qstar_string_list_free(inputs);
			return -1;
		}
	}
	return 0;
}
