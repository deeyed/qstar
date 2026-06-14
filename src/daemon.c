#if defined(__APPLE__) && defined(__MACH__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "internal.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define QSTAR_DAEMON_BUILD_MAGIC "qstar-daemon-build-v1"
#define QSTAR_DAEMON_HELLO_MAGIC "qstar-daemon-hello-v1"
#define QSTAR_DAEMON_RESPONSE_MAGIC "qstar-daemon-response-v1"
#define QSTAR_DAEMON_MAX_BODY (64U * 1024U * 1024U)

struct qstar_daemon_request {
	char cwd[QSTAR_PATH_MAX];
	char file[QSTAR_PATH_MAX];
	char label[QSTAR_PATH_MAX];
	char build_dir[QSTAR_PATH_MAX];
	char profile[128];
	char target[128];
	char toolchain[128];
	char stdlib_policy[128];
	struct qstar_build_options options;
};

struct qstar_daemon_fp {
	char *path;
	unsigned long long size;
	unsigned long long mtime;
};

struct qstar_daemon_server {
	struct qstar_graph graph;
	int graph_init;
	int graph_loaded;
	char cwd[QSTAR_PATH_MAX];
	char file[QSTAR_PATH_MAX];
	char label[QSTAR_PATH_MAX];
	char build_dir[QSTAR_PATH_MAX];
	char profile[128];
	char target[128];
	char toolchain[128];
	char stdlib_policy[128];
	struct qstar_daemon_fp *fps;
	size_t fp_len;
	size_t fp_cap;
};

/** daemon/client error buffer에 printf 형식 메시지를 기록한다. */
static void
set_error(char *error, size_t error_len, const char *fmt, ...)
{
	va_list ap;

	if (!error || error_len == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(error, error_len, fmt, ap);
	va_end(ap);
}

/** nullable string을 고정 크기 buffer로 안전하게 복사한다. */
static int
copy_string(char *dst, size_t dstlen, const char *src)
{
	if (snprintf(dst, dstlen, "%s", src ? src : "") >= (int)dstlen)
		return -1;
	return 0;
}

/** fd에 지정한 byte 수 전체를 쓸 때까지 반복한다. */
static int
write_all(int fd, const void *buf, size_t len)
{
	const char *p;
	ssize_t n;

	p = buf;
	while (len > 0) {
		n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/** fd에서 지정한 byte 수 전체를 읽을 때까지 반복한다. */
static int
read_exact(int fd, void *buf, size_t len)
{
	char *p;
	ssize_t n;

	p = buf;
	while (len > 0) {
		n = read(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/** daemon line protocol의 한 줄을 읽고 개행을 제거한다. */
static int
read_line(int fd, char *dst, size_t dstlen)
{
	size_t n;
	char c;
	ssize_t r;

	if (dstlen == 0)
		return -1;
	n = 0;
	for (;;) {
		r = read(fd, &c, 1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return -1;
		if (c == '\n')
			break;
		if (c == '\r')
			continue;
		if (n + 1 >= dstlen)
			return -1;
		dst[n++] = c;
	}
	dst[n] = '\0';
	return 0;
}

/** daemon line protocol의 한 줄을 쓴다. */
static int
write_line(int fd, const char *s)
{
	return write_all(fd, s, strlen(s)) < 0 || write_all(fd, "\n", 1) < 0 ? -1 : 0;
}

/** Unix socket directory 생성을 위해 parent path를 재귀적으로 만든다. */
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
			if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

/** 파일 path의 parent directory를 재귀적으로 만든다. */
static int
mkdir_parent(const char *path)
{
	char dir[QSTAR_PATH_MAX];

	if (qstar_dirname(path, dir, sizeof(dir)) < 0)
		return -1;
	return mkdir_p(dir);
}

/** CLI override 기준의 default experimental daemon socket path를 계산한다. */
static int
default_socket_path(const char *cli_build_dir, char *dst, size_t dstlen)
{
	char cwd[QSTAR_PATH_MAX];
	const char *build_dir;

	if (!getcwd(cwd, sizeof(cwd)))
		return -1;
	build_dir = cli_build_dir && *cli_build_dir ? cli_build_dir : "build/qstar";
	if (snprintf(dst, dstlen, "%s/%s/stella/daemon/qstar-daemon.sock",
	    cwd, build_dir) >= (int)dstlen)
		return -1;
	return 0;
}

/** Unix domain socket에 연결하고 실패 이유를 error buffer에 기록한다. */
static int
connect_socket(const char *socket_path, char *error, size_t error_len)
{
	struct sockaddr_un addr;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		set_error(error, error_len, "socket: %s", strerror(errno));
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(socket_path) >= sizeof(addr.sun_path)) {
		set_error(error, error_len, "socket path too long: %s", socket_path);
		close(fd);
		return -1;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		set_error(error, error_len, "connect %s: %s", socket_path, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

/** CLI daemon mode 문자열을 experimental daemon policy로 변환한다. */
int
qstar_daemon_parse_mode(const char *s, int *mode)
{
	if (strcmp(s, "never") == 0 || strcmp(s, "off") == 0)
		*mode = QSTAR_DAEMON_NEVER;
	else if (strcmp(s, "auto") == 0)
		*mode = QSTAR_DAEMON_AUTO;
	else if (strcmp(s, "always") == 0 || strcmp(s, "on") == 0)
		*mode = QSTAR_DAEMON_ALWAYS;
	else
		return -1;
	return 0;
}

/** request line 하나를 nullable-safe 값으로 전송한다. */
static int
send_request_line(int fd, const char *value)
{
	return write_line(fd, value ? value : "");
}

/** daemon build request를 line protocol로 전송한다. */
static int
send_build_request(int fd, const struct qstar_daemon_request *req)
{
	char line[64];

	if (write_line(fd, QSTAR_DAEMON_BUILD_MAGIC) < 0)
		return -1;
	if (send_request_line(fd, req->cwd) < 0 ||
	    send_request_line(fd, req->file) < 0 ||
	    send_request_line(fd, req->label) < 0 ||
	    send_request_line(fd, req->build_dir) < 0 ||
	    send_request_line(fd, req->profile) < 0 ||
	    send_request_line(fd, req->target) < 0 ||
	    send_request_line(fd, req->toolchain) < 0 ||
	    send_request_line(fd, req->stdlib_policy) < 0)
		return -1;
	snprintf(line, sizeof(line), "%d", req->options.jobs);
	if (write_line(fd, line) < 0)
		return -1;
	snprintf(line, sizeof(line), "%d", req->options.schedule_trace);
	if (write_line(fd, line) < 0)
		return -1;
	snprintf(line, sizeof(line), "%d", req->options.verbose);
	if (write_line(fd, line) < 0)
		return -1;
	snprintf(line, sizeof(line), "%d", req->options.quiet);
	if (write_line(fd, line) < 0)
		return -1;
	snprintf(line, sizeof(line), "%d", req->options.progress_mode);
	if (write_line(fd, line) < 0)
		return -1;
	snprintf(line, sizeof(line), "%d", req->options.color_mode);
	return write_line(fd, line);
}

/** line protocol의 정수 값을 검증하며 파싱한다. */
static int
parse_int_line(const char *line, int *value)
{
	char tail;

	if (sscanf(line, "%d%c", value, &tail) != 1)
		return -1;
	return 0;
}

/** daemon server가 build request 본문을 구조체로 읽는다. */
static int
read_request(int fd, struct qstar_daemon_request *req)
{
	char line[QSTAR_PATH_MAX];

	memset(req, 0, sizeof(*req));
	if (read_line(fd, req->cwd, sizeof(req->cwd)) < 0 ||
	    read_line(fd, req->file, sizeof(req->file)) < 0 ||
	    read_line(fd, req->label, sizeof(req->label)) < 0 ||
	    read_line(fd, req->build_dir, sizeof(req->build_dir)) < 0 ||
	    read_line(fd, req->profile, sizeof(req->profile)) < 0 ||
	    read_line(fd, req->target, sizeof(req->target)) < 0 ||
	    read_line(fd, req->toolchain, sizeof(req->toolchain)) < 0 ||
	    read_line(fd, req->stdlib_policy, sizeof(req->stdlib_policy)) < 0)
		return -1;
	if (read_line(fd, line, sizeof(line)) < 0 ||
	    parse_int_line(line, &req->options.jobs) < 0 ||
	    read_line(fd, line, sizeof(line)) < 0 ||
	    parse_int_line(line, &req->options.schedule_trace) < 0 ||
	    read_line(fd, line, sizeof(line)) < 0 ||
	    parse_int_line(line, &req->options.verbose) < 0 ||
	    read_line(fd, line, sizeof(line)) < 0 ||
	    parse_int_line(line, &req->options.quiet) < 0 ||
	    read_line(fd, line, sizeof(line)) < 0 ||
	    parse_int_line(line, &req->options.progress_mode) < 0 ||
	    read_line(fd, line, sizeof(line)) < 0 ||
	    parse_int_line(line, &req->options.color_mode) < 0)
		return -1;
	return 0;
}

/** daemon response body를 client stdout으로 복사하고 build status를 반환한다. */
static int
read_response(int fd, FILE *out, int *build_status, char *error, size_t error_len)
{
	char line[256], *end;
	unsigned long body_len, left, chunk;
	char buf[8192];
	int status;

	if (read_line(fd, line, sizeof(line)) < 0) {
		set_error(error, error_len, "daemon response missing");
		return -1;
	}
	if (strcmp(line, QSTAR_DAEMON_RESPONSE_MAGIC) != 0) {
		set_error(error, error_len, "daemon response has invalid magic");
		return -1;
	}
	if (read_line(fd, line, sizeof(line)) < 0 ||
	    sscanf(line, "status %d", &status) != 1) {
		set_error(error, error_len, "daemon response missing status");
		return -1;
	}
	if (read_line(fd, line, sizeof(line)) < 0 ||
	    strncmp(line, "bytes ", 6) != 0) {
		set_error(error, error_len, "daemon response missing byte count");
		return -1;
	}
	errno = 0;
	body_len = strtoul(line + 6, &end, 10);
	if (errno != 0 || *end != '\0' || body_len > QSTAR_DAEMON_MAX_BODY) {
		set_error(error, error_len, "daemon response byte count is invalid");
		return -1;
	}
	if (read_line(fd, line, sizeof(line)) < 0 || line[0] != '\0') {
		set_error(error, error_len, "daemon response header terminator missing");
		return -1;
	}
	left = body_len;
	while (left > 0) {
		chunk = left > sizeof(buf) ? (unsigned long)sizeof(buf) : left;
		if (read_exact(fd, buf, chunk) < 0) {
			set_error(error, error_len, "daemon response body truncated");
			return -1;
		}
		if (chunk > 0 && fwrite(buf, 1, chunk, out) != chunk) {
			set_error(error, error_len, "could not write daemon response");
			return -1;
		}
		left -= chunk;
	}
	*build_status = status;
	return 0;
}

/** build request를 experimental daemon으로 보내고 응답 output을 out에 복사한다. */
int
qstar_daemon_build_client(const char *socket_path, int mode, const char *file,
    const char *label, const char *cli_build_dir, const char *cli_profile,
    const char *cli_target, const char *cli_toolchain, const char *cli_stdlib,
    const struct qstar_build_options *options, FILE *out, int *build_status,
    char *error, size_t error_len)
{
	struct qstar_daemon_request req;
	char socket_buf[QSTAR_PATH_MAX];
	int fd;

	(void)mode;
	error[0] = '\0';
	if (!socket_path || !*socket_path) {
		if (default_socket_path(cli_build_dir, socket_buf, sizeof(socket_buf)) < 0) {
			set_error(error, error_len, "could not compute default daemon socket path");
			return -1;
		}
		socket_path = socket_buf;
	}
	fd = connect_socket(socket_path, error, error_len);
	if (fd < 0)
		return -1;
	memset(&req, 0, sizeof(req));
	if (!getcwd(req.cwd, sizeof(req.cwd))) {
		set_error(error, error_len, "getcwd: %s", strerror(errno));
		close(fd);
		return -1;
	}
	if (copy_string(req.file, sizeof(req.file), file) < 0 ||
	    copy_string(req.label, sizeof(req.label), label) < 0 ||
	    copy_string(req.build_dir, sizeof(req.build_dir), cli_build_dir) < 0 ||
	    copy_string(req.profile, sizeof(req.profile), cli_profile) < 0 ||
	    copy_string(req.target, sizeof(req.target), cli_target) < 0 ||
	    copy_string(req.toolchain, sizeof(req.toolchain), cli_toolchain) < 0 ||
	    copy_string(req.stdlib_policy, sizeof(req.stdlib_policy), cli_stdlib) < 0) {
		set_error(error, error_len, "daemon request field is too long");
		close(fd);
		return -1;
	}
	req.options = *options;
	if (send_build_request(fd, &req) < 0) {
		set_error(error, error_len, "could not send daemon request: %s",
		    strerror(errno));
		close(fd);
		return -1;
	}
	if (read_response(fd, out, build_status, error, error_len) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/** daemon authoring fingerprint table을 해제한다. */
static void
free_fps(struct qstar_daemon_server *server)
{
	size_t i;

	for (i = 0; i < server->fp_len; i++)
		free(server->fps[i].path);
	free(server->fps);
	server->fps = NULL;
	server->fp_len = 0;
	server->fp_cap = 0;
}

/** daemon server가 소유한 graph와 memory cache를 해제한다. */
static void
server_free(struct qstar_daemon_server *server)
{
	free_fps(server);
	if (server->graph_init)
		qstar_graph_free(&server->graph);
	memset(server, 0, sizeof(*server));
}

/** stat 결과에서 platform별 nanosecond mtime 값을 계산한다. */
static unsigned long long
stat_mtime_ns(const struct stat *st)
{
#if defined(__APPLE__) && defined(__MACH__)
	return (unsigned long long)st->st_mtimespec.tv_sec * 1000000000ULL +
	    (unsigned long long)st->st_mtimespec.tv_nsec;
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L
	return (unsigned long long)st->st_mtim.tv_sec * 1000000000ULL +
	    (unsigned long long)st->st_mtim.tv_nsec;
#else
	return (unsigned long long)st->st_mtime * 1000000000ULL;
#endif
}

/** authoring input fingerprint용 size/mtime 정보를 읽는다. */
static int
stat_fp(const char *path, unsigned long long *size, unsigned long long *mtime)
{
	struct stat st;

	if (stat(path, &st) < 0)
		return -1;
	*size = (unsigned long long)st.st_size;
	*mtime = stat_mtime_ns(&st);
	return 0;
}

/** package-relative authoring input을 graph package root 기준 full path로 바꾼다. */
static int
input_full_path(const struct qstar_graph *graph, const char *rel, char *dst,
    size_t dstlen)
{
	const char *root;

	root = graph->package_root && *graph->package_root ? graph->package_root : ".";
	if (strcmp(root, ".") == 0)
		return snprintf(dst, dstlen, "%s", rel) < (int)dstlen ? 0 : -1;
	return snprintf(dst, dstlen, "%s/%s", root, rel) < (int)dstlen ? 0 : -1;
}

/** daemon server fingerprint table에 새 path fingerprint를 추가한다. */
static int
add_fp(struct qstar_daemon_server *server, const char *path, unsigned long long size,
    unsigned long long mtime)
{
	struct qstar_daemon_fp *items;
	size_t ncap;

	if (server->fp_len == server->fp_cap) {
		ncap = server->fp_cap ? server->fp_cap * 2 : 8;
		items = realloc(server->fps, ncap * sizeof(items[0]));
		if (!items)
			return -1;
		server->fps = items;
		server->fp_cap = ncap;
	}
	server->fps[server->fp_len].path = qstar_strdup(path);
	if (!server->fps[server->fp_len].path)
		return -1;
	server->fps[server->fp_len].size = size;
	server->fps[server->fp_len].mtime = mtime;
	server->fp_len++;
	return 0;
}

/** 현재 graph의 evaluated authoring inputs를 daemon memory fingerprint로 저장한다. */
static int
capture_authoring_fps(struct qstar_daemon_server *server)
{
	char path[QSTAR_PATH_MAX];
	unsigned long long size, mtime;
	size_t i;

	free_fps(server);
	for (i = 0; i < server->graph.evaluated_fragments.len; i++) {
		if (input_full_path(&server->graph, server->graph.evaluated_fragments.items[i],
		    path, sizeof(path)) < 0 ||
		    stat_fp(path, &size, &mtime) < 0 ||
		    add_fp(server, path, size, mtime) < 0)
			return -1;
	}
	return 0;
}

/** 저장된 authoring input fingerprint가 현재 파일 상태와 달라졌는지 확인한다. */
static int
authoring_inputs_changed(const struct qstar_daemon_server *server)
{
	unsigned long long size, mtime;
	size_t i;

	for (i = 0; i < server->fp_len; i++) {
		if (stat_fp(server->fps[i].path, &size, &mtime) < 0)
			return 1;
		if (server->fps[i].size != size || server->fps[i].mtime != mtime)
			return 1;
	}
	return 0;
}

/** daemon memory graph가 현재 build request identity와 같은지 확인한다. */
static int
same_identity(const struct qstar_daemon_server *server,
    const struct qstar_daemon_request *req)
{
	return strcmp(server->cwd, req->cwd) == 0 &&
	    strcmp(server->file, req->file) == 0 &&
	    strcmp(server->label, req->label) == 0 &&
	    strcmp(server->build_dir, req->build_dir) == 0 &&
	    strcmp(server->profile, req->profile) == 0 &&
	    strcmp(server->target, req->target) == 0 &&
	    strcmp(server->toolchain, req->toolchain) == 0 &&
	    strcmp(server->stdlib_policy, req->stdlib_policy) == 0;
}

/** daemon memory에 Graph IR와 lowered plan cache 결과를 load한다. */
static int
load_graph(struct qstar_daemon_server *server, const struct qstar_daemon_request *req,
    FILE *out, const char **graph_status, const char **reason)
{
	char cache_reason[128], store_reason[128];
	int rc, plan_loaded;

	*graph_status = "miss";
	*reason = "cold";
	if (server->graph_init)
		qstar_graph_free(&server->graph);
	qstar_graph_init(&server->graph);
	server->graph_init = 1;
	server->graph_loaded = 0;
	rc = qstar_graph_set_profile_input(&server->graph,
	    req->profile[0] ? req->profile : NULL, NULL, NULL, NULL);
	if (rc == 0)
		rc = qstar_graph_set_cli_overrides(&server->graph, "stella",
		    req->build_dir[0] ? req->build_dir : NULL);
	cache_reason[0] = '\0';
	plan_loaded = 0;
	if (rc == 0) {
		plan_loaded = qstar_stella_plan_cache_try_load(&server->graph, req->file,
		    "build", req->label[0] ? req->label : NULL,
		    req->profile[0] ? req->profile : NULL,
		    req->target[0] ? req->target : NULL,
		    req->toolchain[0] ? req->toolchain : NULL,
		    req->stdlib_policy[0] ? req->stdlib_policy : NULL,
		    cache_reason, sizeof(cache_reason));
	}
	if (rc == 0 && !plan_loaded)
		rc = qstar_lua_eval_file(&server->graph, req->file);
	if (rc == 0 && !plan_loaded)
		rc = qstar_graph_set_cli_overrides(&server->graph, "stella",
		    req->build_dir[0] ? req->build_dir : NULL);
	if (rc == 0 && !plan_loaded)
		rc = qstar_graph_apply_selected_profile(&server->graph);
	if (rc == 0 && !plan_loaded)
		rc = qstar_graph_set_profile_input(&server->graph,
		    req->profile[0] ? req->profile : NULL,
		    req->target[0] ? req->target : NULL,
		    req->toolchain[0] ? req->toolchain : NULL,
		    req->stdlib_policy[0] ? req->stdlib_policy : NULL);
	if (rc == 0 && !plan_loaded)
		rc = qstar_graph_validate_profile(&server->graph);
	if (rc == 0 && !plan_loaded)
		rc = qstar_graph_validate_packages(&server->graph);
	if (rc == 0 && !plan_loaded)
		rc = qstar_graph_validate_generated_outputs(&server->graph);
	if (rc == 0 && !plan_loaded)
		rc = qstar_graph_validate_sources(&server->graph);
	if (rc == 0 && !plan_loaded)
		rc = qstar_graph_validate_headers(&server->graph);
	if (rc == 0 && !plan_loaded)
		rc = qstar_graph_validate_file_inputs(&server->graph);
	if (rc == 0 && !plan_loaded) {
		store_reason[0] = '\0';
		if (qstar_stella_plan_cache_store(&server->graph, req->file, "build",
		    req->label[0] ? req->label : NULL,
		    req->profile[0] ? req->profile : NULL,
		    req->target[0] ? req->target : NULL,
		    req->toolchain[0] ? req->toolchain : NULL,
		    req->stdlib_policy[0] ? req->stdlib_policy : NULL,
		    store_reason, sizeof(store_reason)) < 0) {
			rc = qstar_set_error(&server->graph,
			    "qstar: could not write Stella plan cache: %s",
			    store_reason[0] ? store_reason : "unknown");
		}
	}
	if (rc == 0 && capture_authoring_fps(server) < 0)
		rc = qstar_set_error(&server->graph,
		    "qstar: could not fingerprint daemon authoring inputs");
	if (rc < 0) {
		fprintf(out, "%s\n", server->graph.error[0] ? server->graph.error :
		    "qstar: daemon graph load failed");
		return -1;
	}
	copy_string(server->cwd, sizeof(server->cwd), req->cwd);
	copy_string(server->file, sizeof(server->file), req->file);
	copy_string(server->label, sizeof(server->label), req->label);
	copy_string(server->build_dir, sizeof(server->build_dir), req->build_dir);
	copy_string(server->profile, sizeof(server->profile), req->profile);
	copy_string(server->target, sizeof(server->target), req->target);
	copy_string(server->toolchain, sizeof(server->toolchain), req->toolchain);
	copy_string(server->stdlib_policy, sizeof(server->stdlib_policy), req->stdlib_policy);
	server->graph_loaded = 1;
	*reason = plan_loaded ? "plan-cache-hit" :
	    (cache_reason[0] ? cache_reason : "evaluated");
	return 0;
}

/** request를 처리하기 전에 daemon memory graph hit/miss와 invalidation을 결정한다. */
static int
ensure_graph(struct qstar_daemon_server *server, const struct qstar_daemon_request *req,
    FILE *out, const char **graph_status, const char **reason)
{
	if (!server->graph_loaded)
		return load_graph(server, req, out, graph_status, reason);
	if (!same_identity(server, req)) {
		*reason = "identity-changed";
		return load_graph(server, req, out, graph_status, reason);
	}
	if (authoring_inputs_changed(server)) {
		*reason = "authoring-input-changed";
		return load_graph(server, req, out, graph_status, reason);
	}
	*graph_status = "hit";
	*reason = "memory";
	return 0;
}

/** tmpfile body를 daemon response header와 함께 client fd로 전송한다. */
static int
send_file_response(int fd, int status, FILE *body)
{
	char header[128], buf[8192];
	long len, left, nread;

	if (fflush(body) != 0 ||
	    fseek(body, 0, SEEK_END) != 0 ||
	    (len = ftell(body)) < 0 ||
	    fseek(body, 0, SEEK_SET) != 0)
		return -1;
	if (write_line(fd, QSTAR_DAEMON_RESPONSE_MAGIC) < 0)
		return -1;
	snprintf(header, sizeof(header), "status %d", status);
	if (write_line(fd, header) < 0)
		return -1;
	snprintf(header, sizeof(header), "bytes %ld", len);
	if (write_line(fd, header) < 0 || write_line(fd, "") < 0)
		return -1;
	left = len;
	while (left > 0) {
		nread = (long)fread(buf, 1, left > (long)sizeof(buf) ?
		    sizeof(buf) : (size_t)left, body);
		if (nread <= 0)
			return -1;
		if (write_all(fd, buf, (size_t)nread) < 0)
			return -1;
		left -= nread;
	}
	return 0;
}

/** build request 하나를 Stella executor로 처리하고 response를 전송한다. */
static int
handle_build_request(struct qstar_daemon_server *server, int fd)
{
	struct qstar_daemon_request req;
	char oldcwd[QSTAR_PATH_MAX];
	const char *graph_status, *reason;
	FILE *body;
	int rc;

	body = tmpfile();
	if (!body)
		return -1;
	if (read_request(fd, &req) < 0) {
		fprintf(body, "qstar: invalid daemon request\n");
		rc = 1;
		goto send;
	}
	if (!getcwd(oldcwd, sizeof(oldcwd))) {
		fprintf(body, "qstar: daemon getcwd failed: %s\n", strerror(errno));
		rc = 1;
		goto send;
	}
	if (chdir(req.cwd) < 0) {
		fprintf(body, "qstar: daemon could not enter workspace '%s': %s\n",
		    req.cwd, strerror(errno));
		rc = 1;
		goto send_restore;
	}
	graph_status = "miss";
	reason = "cold";
	if (ensure_graph(server, &req, body, &graph_status, &reason) < 0) {
		rc = 1;
		goto send_restore;
	}
	if (req.options.schedule_trace || req.options.verbose)
		fprintf(body,
		    "daemon_server status=build graph=%s reason=%s experimental=1\n",
		    graph_status, reason);
	rc = qstar_graph_build_with_options(&server->graph,
	    req.label[0] ? req.label : NULL, &req.options, body) < 0 ? 1 : 0;
	if (rc != 0 && server->graph.error[0])
		fprintf(body, "%s\n", server->graph.error);
send_restore:
	if (chdir(oldcwd) < 0 && rc == 0) {
		fprintf(body, "qstar: daemon could not restore cwd: %s\n", strerror(errno));
		rc = 1;
	}
send:
	if (send_file_response(fd, rc, body) < 0)
		rc = 1;
	fclose(body);
	return rc == 0 ? 0 : -1;
}

/** 짧은 text response를 daemon protocol로 전송한다. */
static int
send_simple_response(int fd, int status, const char *text)
{
	FILE *body;
	int rc;

	body = tmpfile();
	if (!body)
		return -1;
	fputs(text, body);
	rc = send_file_response(fd, status, body);
	fclose(body);
	return rc;
}

/** foreground experimental daemon server를 Unix domain socket으로 실행한다. */
static int
serve_socket(const char *socket_path, FILE *out)
{
	struct qstar_daemon_server server;
	struct sockaddr_un addr;
	char magic[128];
	int listen_fd, client_fd;

	memset(&server, 0, sizeof(server));
	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		fprintf(stderr, "qstar: daemon socket failed: %s\n", strerror(errno));
		return 1;
	}
	if (mkdir_parent(socket_path) < 0) {
		fprintf(stderr, "qstar: daemon could not create socket directory\n");
		close(listen_fd);
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(socket_path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "qstar: daemon socket path is too long: %s\n", socket_path);
		close(listen_fd);
		return 1;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
	unlink(socket_path);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(listen_fd, 16) < 0) {
		fprintf(stderr, "qstar: daemon listen failed: %s\n", strerror(errno));
		close(listen_fd);
		return 1;
	}
	fprintf(out, "qstar daemon experimental socket=%s\n", socket_path);
	fflush(out);
	for (;;) {
		client_fd = accept(listen_fd, NULL, NULL);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "qstar: daemon accept failed: %s\n", strerror(errno));
			break;
		}
		if (read_line(client_fd, magic, sizeof(magic)) == 0) {
			if (strcmp(magic, QSTAR_DAEMON_BUILD_MAGIC) == 0)
				(void)handle_build_request(&server, client_fd);
			else if (strcmp(magic, QSTAR_DAEMON_HELLO_MAGIC) == 0)
				(void)send_simple_response(client_fd, 0,
				    "daemon status=ok experimental=1\n");
			else
				(void)send_simple_response(client_fd, 1,
				    "qstar: unknown daemon request\n");
		}
		close(client_fd);
	}
	server_free(&server);
	close(listen_fd);
	return 1;
}

/** daemon socket에 hello request를 보내 status를 확인한다. */
static int
daemon_status(const char *socket_path, FILE *out)
{
	char error[256];
	int fd, status;

	fd = connect_socket(socket_path, error, sizeof(error));
	if (fd < 0) {
		fprintf(out, "daemon status=unavailable reason=%s\n", error);
		return 1;
	}
	if (write_line(fd, QSTAR_DAEMON_HELLO_MAGIC) < 0 ||
	    read_response(fd, out, &status, error, sizeof(error)) < 0) {
		fprintf(out, "daemon status=unavailable reason=%s\n",
		    error[0] ? error : "protocol-error");
		close(fd);
		return 1;
	}
	close(fd);
	return status;
}

/** experimental daemon subcommand usage를 출력한다. */
static void
daemon_usage(FILE *out)
{
	fputs("usage: qstar [options] daemon --socket path --serve\n", out);
	fputs("       qstar [options] daemon --socket path --status\n", out);
	fputs("Experimental Stella daemon. The stable build path is unchanged.\n", out);
}

/** experimental persistent Stella daemon command를 실행한다. */
int
qstar_daemon_command(int argc, char **argv, const char *file,
    const char *cli_build_dir, FILE *out)
{
	const char *socket_path;
	char socket_buf[QSTAR_PATH_MAX];
	int serve, status, i;

	(void)file;
	socket_path = NULL;
	serve = 0;
	status = 0;
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--socket") == 0) {
			if (i + 1 >= argc) {
				daemon_usage(stderr);
				return 2;
			}
			socket_path = argv[++i];
		} else if (strcmp(argv[i], "--serve") == 0 ||
		    strcmp(argv[i], "start") == 0) {
			serve = 1;
		} else if (strcmp(argv[i], "--status") == 0 ||
		    strcmp(argv[i], "status") == 0) {
			status = 1;
		} else if (strcmp(argv[i], "--help") == 0 ||
		    strcmp(argv[i], "-h") == 0) {
			daemon_usage(out);
			return 0;
		} else {
			daemon_usage(stderr);
			return 2;
		}
	}
	if (serve + status != 1) {
		daemon_usage(stderr);
		return 2;
	}
	if (!socket_path || !*socket_path) {
		if (default_socket_path(cli_build_dir, socket_buf, sizeof(socket_buf)) < 0) {
			fprintf(stderr, "qstar: could not compute default daemon socket path\n");
			return 1;
		}
		socket_path = socket_buf;
	}
	if (serve)
		return serve_socket(socket_path, out);
	return daemon_status(socket_path, out);
}
