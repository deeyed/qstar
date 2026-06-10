#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct init_file {
	const char *path;
	const char *body;
	int executable;
};

static const struct init_file c_app_files[] = {
	{ "qstar.lua",
	  "qstar.project {\n"
	  "  name = \"c-app\",\n"
	  "  version = \"0.1.0\",\n"
	  "  root = \".\",\n"
	  "}\n"
	  "\n"
	  "qstar.executable \"app\" {\n"
	  "  sources = {\"src/main.c\"},\n"
	  "}\n", 0 },
	{ "src/main.c",
	  "/** qstar init c-app sample executable 진입점이다. */\n"
	  "int main(void)\n"
	  "{\n"
	  "\treturn 0;\n"
	  "}\n", 0 },
};

static const struct init_file c_lib_files[] = {
	{ "qstar.lua",
	  "qstar.project {\n"
	  "  name = \"c-lib\",\n"
	  "  version = \"0.1.0\",\n"
	  "  root = \".\",\n"
	  "}\n"
	  "\n"
	  "qstar.staticlib \"core\" {\n"
	  "  sources = {\"src/core.c\"},\n"
	  "  lang = {\n"
	  "    c = {\n"
	  "      public_headers = {\"include/core.h\"},\n"
	  "      public_include_dirs = {\"include\"},\n"
	  "    },\n"
	  "  },\n"
	  "}\n"
	  "\n"
	  "qstar.test \"unit\" {\n"
	  "  sources = {\"tests/unit.c\"},\n"
	  "  deps = {\"//:core\"},\n"
	  "}\n", 0 },
	{ "include/core.h",
	  "#ifndef QSTAR_INIT_CORE_H\n"
	  "#define QSTAR_INIT_CORE_H\n"
	  "\n"
	  "int core_add(int lhs, int rhs);\n"
	  "\n"
	  "#endif\n", 0 },
	{ "src/core.c",
	  "#include \"core.h\"\n"
	  "\n"
	  "/** 두 정수의 합을 반환하는 qstar init c-lib sample 함수다. */\n"
	  "int core_add(int lhs, int rhs)\n"
	  "{\n"
	  "\treturn lhs + rhs;\n"
	  "}\n", 0 },
	{ "tests/unit.c",
	  "#include \"core.h\"\n"
	  "\n"
	  "/** qstar init c-lib sample library를 검증하는 test 진입점이다. */\n"
	  "int main(void)\n"
	  "{\n"
	  "\treturn core_add(20, 22) == 42 ? 0 : 1;\n"
	  "}\n", 0 },
};

static const struct init_file generated_files[] = {
	{ "qstar.lua",
	  "qstar.project {\n"
	  "  name = \"generated\",\n"
	  "  version = \"0.1.0\",\n"
	  "  root = \".\",\n"
	  "}\n"
	  "\n"
	  "qstar.configure_file \"cfg\" {\n"
	  "  output = qstar.output(\"generated/config.h\"),\n"
	  "  defines = {\"APP_VALUE=42\", \"APP_FEATURE\"},\n"
	  "}\n"
	  "\n"
	  "qstar.custom_target \"generated_value\" {\n"
	  "  outputs = {qstar.output(\"generated/value.c\")},\n"
	  "  command = qstar.cli {\"tools/gen-value.sh\", qstar.output(0)},\n"
	  "}\n"
	  "\n"
	  "qstar.executable \"app\" {\n"
	  "  sources = {\"src/main.c\", qstar.output(\"generated/value.c\")},\n"
	  "  lang = {\n"
	  "    c = {\n"
	  "      private_headers = {qstar.output(\"generated/config.h\")},\n"
	  "      include_dirs = {\"generated\"},\n"
	  "    },\n"
	  "  },\n"
	  "}\n", 0 },
	{ "src/main.c",
	  "#include \"config.h\"\n"
	  "\n"
	  "int generated_value(void);\n"
	  "\n"
	  "/** generated config header sample executable 진입점이다. */\n"
	  "int main(void)\n"
	  "{\n"
	  "\treturn generated_value() - APP_VALUE;\n"
	  "}\n", 0 },
	{ "tools/gen-value.sh",
	  "#!/bin/sh\n"
	  "set -eu\n"
	  "\n"
	  "out=$1\n"
	  "mkdir -p \"$(dirname \"$out\")\"\n"
	  "cat > \"$out\" <<'SRC'\n"
	  "int generated_value(void)\n"
	  "{\n"
	  "\treturn 42;\n"
	  "}\n"
	  "SRC\n", 1 },
};

static const struct init_file mixed_cale_files[] = {
	{ "qstar.lua",
	  "qstar.project {\n"
	  "  name = \"mixed-cale\",\n"
	  "  version = \"0.1.0\",\n"
	  "  root = \".\",\n"
	  "}\n"
	  "\n"
	  "qstar.executable \"mixed\" {\n"
	  "  toolchain = \"cale\",\n"
	  "  sources = {\"src/main.c\", \"src/plugin.cale\"},\n"
	  "  lang = {\n"
	  "    cale = {\n"
	  "      profile = \"safe\",\n"
	  "      compile_options = {},\n"
	  "      public_include_dirs = {},\n"
	  "    },\n"
	  "  },\n"
	  "}\n", 0 },
	{ "src/main.c",
	  "int cale_plugin_value(void);\n"
	  "\n"
	  "/** C/Cale mixed sample executable의 C 진입점이다. */\n"
	  "int main(void)\n"
	  "{\n"
	  "\treturn cale_plugin_value() == 9 ? 0 : 1;\n"
	  "}\n", 0 },
	{ "src/plugin.cale",
	  "fn cale_plugin_value() -> int {\n"
	  "    return 9;\n"
	  "}\n", 0 },
};

struct init_template {
	const char *name;
	const struct init_file *files;
	size_t len;
};

static const struct init_template templates[] = {
	{ "c-app", c_app_files, sizeof(c_app_files) / sizeof(c_app_files[0]) },
	{ "c-lib", c_lib_files, sizeof(c_lib_files) / sizeof(c_lib_files[0]) },
	{ "generated", generated_files, sizeof(generated_files) / sizeof(generated_files[0]) },
	{ "mixed-cale", mixed_cale_files, sizeof(mixed_cale_files) / sizeof(mixed_cale_files[0]) },
};

/** init error buffer에 stable message를 기록한다. */
static int
init_error(char *error, size_t error_len, const char *fmt, const char *arg)
{
	if (error && error_len)
		snprintf(error, error_len, fmt, arg);
	return -1;
}

/** path가 이미 존재하는지 확인한다. */
static int
path_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/** directory를 부모부터 생성한다. */
static int
mkdir_p(const char *path, char *error, size_t error_len)
{
	char buf[QSTAR_PATH_MAX];
	size_t i, n;

	n = strlen(path);
	if (n >= sizeof(buf))
		return init_error(error, error_len, "qstar: init path too long '%s'", path);
	memcpy(buf, path, n + 1);
	for (i = 1; i < n; i++) {
		if (buf[i] != '/')
			continue;
		buf[i] = '\0';
		if (buf[0] && mkdir(buf, 0777) < 0 && errno != EEXIST)
			return init_error(error, error_len,
			    "qstar: init could not create directory '%s'", buf);
		buf[i] = '/';
	}
	if (mkdir(buf, 0777) < 0 && errno != EEXIST)
		return init_error(error, error_len,
		    "qstar: init could not create directory '%s'", buf);
	return 0;
}

/** file path의 parent directory를 생성한다. */
static int
mkdir_parent(const char *path, char *error, size_t error_len)
{
	char dir[QSTAR_PATH_MAX];
	char *slash;

	if (strlen(path) >= sizeof(dir))
		return init_error(error, error_len, "qstar: init path too long '%s'", path);
	strcpy(dir, path);
	slash = strrchr(dir, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	return mkdir_p(dir, error, error_len);
}

/** template file 하나를 새로 쓴다. 기존 파일은 덮어쓰지 않는다. */
static int
write_template_file(const char *root, const struct init_file *file, FILE *out,
    char *error, size_t error_len)
{
	char path[QSTAR_PATH_MAX];
	FILE *f;
	int failed;

	if (qstar_path_join(root, file->path, path, sizeof(path)) < 0)
		return init_error(error, error_len, "qstar: init path too long '%s'",
		    file->path);
	if (path_exists(path))
		return init_error(error, error_len,
		    "qstar: init refuses to overwrite existing file '%s'", path);
	if (mkdir_parent(path, error, error_len) < 0)
		return -1;
	f = fopen(path, "wb");
	if (!f)
		return init_error(error, error_len,
		    "qstar: init could not create file '%s'", path);
	failed = fputs(file->body, f) < 0;
	if (fclose(f) < 0)
		failed = 1;
	if (failed)
		return init_error(error, error_len,
		    "qstar: init could not write file '%s'", path);
	if (file->executable)
		chmod(path, 0755);
	fprintf(out, "create %s\n", file->path);
	return 0;
}

/** template 이름에 맞는 init template을 찾는다. */
static const struct init_template *
find_template(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(templates) / sizeof(templates[0]); i++) {
		if (strcmp(templates[i].name, name) == 0)
			return &templates[i];
	}
	return NULL;
}

/** qstar init template을 지정된 directory에 생성한다. */
int
qstar_init_project(const char *template_name, const char *directory, FILE *out,
    char *error, size_t error_len)
{
	const struct init_template *template;
	size_t i;

	template = find_template(template_name);
	if (!template)
		return init_error(error, error_len,
		    "qstar: unknown init template '%s'", template_name);
	if (mkdir_p(directory, error, error_len) < 0)
		return -1;
	fprintf(out, "qstar init v1\n");
	fprintf(out, "template %s\n", template->name);
	fprintf(out, "directory %s\n", directory);
	for (i = 0; i < template->len; i++) {
		if (write_template_file(directory, &template->files[i], out, error,
		    error_len) < 0)
			return -1;
	}
	fputs("status ok\n", out);
	return 0;
}
