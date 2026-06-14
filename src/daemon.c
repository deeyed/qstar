#if defined(__APPLE__) && defined(__MACH__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__MACH__)
#include <sys/event.h>
#include <sys/time.h>
#ifndef O_EVTONLY
#define O_EVTONLY O_RDONLY
#endif
#elif defined(__linux__)
#include <sys/inotify.h>
#endif

#define QSTAR_DAEMON_BUILD_MAGIC "qstar-daemon-build-v1"
#define QSTAR_DAEMON_HELLO_MAGIC "qstar-daemon-hello-v1"
#define QSTAR_DAEMON_QUERY_MAGIC "qstar-daemon-query-v1"
#define QSTAR_DAEMON_RESPONSE_MAGIC "qstar-daemon-response-v1"
#define QSTAR_DAEMON_STREAM_MAGIC "qstar-daemon-stream-v1"
#define QSTAR_DAEMON_MAX_BODY (64U * 1024U * 1024U)
#define QSTAR_DAEMON_MAX_EVENT (16U * 1024U * 1024U)
#define QSTAR_DAEMON_LINE_BUF 8192U

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

enum {
	QSTAR_DAEMON_WATCH_AUTHORING = 1 << 0,
	QSTAR_DAEMON_WATCH_INPUT = 1 << 1
};

struct qstar_daemon_watch_entry {
	char *path;
	int scope;
	int fd;
	int wd;
};

struct qstar_daemon_watcher {
	int active;
	int backend_fd;
	char backend[16];
	char reason[96];
	int incomplete;
	int skipped_missing;
	int graph_dirty;
	int input_dirty;
	struct qstar_daemon_watch_entry *entries;
	size_t len;
	size_t cap;
};

struct qstar_daemon_server {
	struct qstar_graph graph;
	int graph_init;
	int graph_loaded;
	struct qstar_stella_state_cache *state_cache;
	char cwd[QSTAR_PATH_MAX];
	char file[QSTAR_PATH_MAX];
	char label[QSTAR_PATH_MAX];
	char build_dir[QSTAR_PATH_MAX];
	char profile[128];
	char target[128];
	char toolchain[128];
	char stdlib_policy[128];
	char graph_reason[128];
	struct qstar_daemon_fp *fps;
	size_t fp_len;
	size_t fp_cap;
	struct qstar_daemon_watcher watcher;
};

struct qstar_daemon_event_stream {
	int fd;
	char line[QSTAR_DAEMON_LINE_BUF];
	size_t line_len;
	int failed;
};

static int input_full_path(const struct qstar_graph *graph, const char *rel,
    char *dst, size_t dstlen);

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

/** JSON string literal을 daemon response에 맞게 escaping해 출력한다. */
static void
daemon_json_string(FILE *out, const char *s)
{
	const unsigned char *p = (const unsigned char *)(s ? s : "");

	fputc('"', out);
	while (*p) {
		if (*p == '"' || *p == '\\')
			fprintf(out, "\\%c", *p);
		else if (*p == '\n')
			fputs("\\n", out);
		else if (*p == '\r')
			fputs("\\r", out);
		else if (*p == '\t')
			fputs("\\t", out);
		else if (*p < 0x20)
			fprintf(out, "\\u%04x", *p);
		else
			fputc(*p, out);
		p++;
	}
	fputc('"', out);
}

/** daemon read API method 이름이 public read-only surface인지 확인한다. */
static int
daemon_query_method_supported(const char *method)
{
	return strcmp(method, "hello") == 0 ||
	    strcmp(method, "workspace.info") == 0 ||
	    strcmp(method, "targets.list") == 0 ||
	    strcmp(method, "diagnostics.list") == 0 ||
	    strcmp(method, "compile_commands.path") == 0 ||
	    strcmp(method, "build.summary") == 0;
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

/** byte buffer가 지정한 prefix로 시작하는지 확인한다. */
static int
bytes_starts_with(const char *buf, size_t len, const char *prefix)
{
	size_t n;

	n = strlen(prefix);
	return len >= n && memcmp(buf, prefix, n) == 0;
}

/** byte buffer 안에 needle이 포함되는지 확인한다. */
static int
bytes_contains(const char *buf, size_t len, const char *needle)
{
	size_t n, i;

	n = strlen(needle);
	if (n == 0 || len < n)
		return 0;
	for (i = 0; i + n <= len; i++) {
		if (memcmp(buf + i, needle, n) == 0)
			return 1;
	}
	return 0;
}

/** daemon stream payload를 client event 종류로 분류한다. */
static const char *
classify_event_payload(const char *buf, size_t len)
{
	if (len == 0)
		return "output";
	if (buf[0] == '[' && bytes_contains(buf, len, "%]"))
		return "progress";
	if (bytes_contains(buf, len, "warning:") || bytes_contains(buf, len, "error:"))
		return "diagnostic";
	if (bytes_starts_with(buf, len, "action_") ||
	    bytes_starts_with(buf, len, "build_action ") ||
	    bytes_starts_with(buf, len, "schedule_action ") ||
	    bytes_starts_with(buf, len, "cache_action ") ||
	    bytes_starts_with(buf, len, "daemon_server ") ||
	    bytes_starts_with(buf, len, "daemon_watcher ") ||
	    bytes_starts_with(buf, len, "plan_cache ") ||
	    bytes_starts_with(buf, len, "dirty_state_db ") ||
	    bytes_starts_with(buf, len, "deps_db "))
		return "action";
	if (bytes_starts_with(buf, len, "qstar build ") ||
	    bytes_starts_with(buf, len, "backend ") ||
	    bytes_starts_with(buf, len, "root ") ||
	    bytes_starts_with(buf, len, "status ") ||
	    bytes_starts_with(buf, len, "run="))
		return "summary";
	return "output";
}

/** daemon event frame 하나를 전송한다. */
static int
write_event_frame(int fd, const char *type, const char *buf, size_t len)
{
	char header[128];

	if (snprintf(header, sizeof(header), "event %s %lu", type,
	    (unsigned long)len) >= (int)sizeof(header))
		return -1;
	if (write_line(fd, header) < 0)
		return -1;
	return len == 0 ? 0 : write_all(fd, buf, len);
}

/** daemon event stream에 buffered line을 frame으로 내보낸다. */
static int
event_stream_flush_line(struct qstar_daemon_event_stream *stream)
{
	const char *type;

	if (stream->line_len == 0)
		return 0;
	type = classify_event_payload(stream->line, stream->line_len);
	if (write_event_frame(stream->fd, type, stream->line, stream->line_len) < 0) {
		stream->failed = 1;
		return -1;
	}
	stream->line_len = 0;
	return 0;
}

/** daemon event stream에 원문 build output byte를 소비시킨다. */
static int
event_stream_write_bytes(struct qstar_daemon_event_stream *stream,
    const char *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (stream->line_len + 1 >= sizeof(stream->line)) {
			if (event_stream_flush_line(stream) < 0)
				return -1;
		}
		stream->line[stream->line_len++] = buf[i];
		if (buf[i] == '\n' && event_stream_flush_line(stream) < 0)
			return -1;
	}
	return 0;
}

#if defined(__APPLE__) && defined(__MACH__)
/** funopen write callback: FILE output을 daemon event frame으로 변환한다. */
static int
event_stream_writefn(void *cookie, const char *buf, int len)
{
	struct qstar_daemon_event_stream *stream;

	stream = cookie;
	if (len <= 0)
		return 0;
	return event_stream_write_bytes(stream, buf, (size_t)len) < 0 ? -1 : len;
}
#elif defined(__linux__)
/** fopencookie write callback: FILE output을 daemon event frame으로 변환한다. */
static ssize_t
event_stream_writefn(void *cookie, const char *buf, size_t len)
{
	struct qstar_daemon_event_stream *stream;

	stream = cookie;
	if (len == 0)
		return 0;
	return event_stream_write_bytes(stream, buf, len) < 0 ? -1 : (ssize_t)len;
}
#endif

/** daemon event stream FILE을 연다. */
static FILE *
open_event_stream(struct qstar_daemon_event_stream *stream, int fd)
{
	FILE *out;

	memset(stream, 0, sizeof(*stream));
	stream->fd = fd;
#if defined(__APPLE__) && defined(__MACH__)
	out = funopen(stream, NULL, event_stream_writefn, NULL, NULL);
#elif defined(__linux__)
	{
		cookie_io_functions_t io;

		memset(&io, 0, sizeof(io));
		io.write = event_stream_writefn;
		out = fopencookie(stream, "w", io);
	}
#else
	out = NULL;
#endif
	if (out)
		setvbuf(out, NULL, _IONBF, 0);
	return out;
}

/** daemon event stream을 종료하기 전에 partial line을 flush한다. */
static int
finish_event_stream(struct qstar_daemon_event_stream *stream, FILE *out)
{
	int rc;

	rc = 0;
	if (out && fflush(out) != 0)
		rc = -1;
	if (event_stream_flush_line(stream) < 0)
		rc = -1;
	return rc;
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

/** daemon request 본문을 line protocol로 전송한다. */
static int
send_request_body(int fd, const struct qstar_daemon_request *req)
{
	char line[64];

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

/** daemon build request를 line protocol로 전송한다. */
static int
send_build_request(int fd, const struct qstar_daemon_request *req)
{
	if (write_line(fd, QSTAR_DAEMON_BUILD_MAGIC) < 0)
		return -1;
	return send_request_body(fd, req);
}

/** daemon read-only query request를 line protocol로 전송한다. */
static int
send_query_request(int fd, const char *method, const struct qstar_daemon_request *req)
{
	if (write_line(fd, QSTAR_DAEMON_QUERY_MAGIC) < 0 ||
	    send_request_line(fd, method) < 0)
		return -1;
	return send_request_body(fd, req);
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

/** daemon server가 read-only query method와 graph request 본문을 읽는다. */
static int
read_query_request(int fd, char *method, size_t method_len,
    struct qstar_daemon_request *req)
{
	if (read_line(fd, method, method_len) < 0)
		return -1;
	return read_request(fd, req);
}

/** daemon byte-count response body를 client stdout으로 복사한다. */
static int
read_buffered_response(int fd, FILE *out, int *build_status, char *error,
    size_t error_len)
{
	char line[256], *end;
	unsigned long body_len, left, chunk;
	char buf[8192];
	int status;

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

/** daemon event stream을 client stdout으로 렌더링하고 최종 build status를 읽는다. */
static int
read_stream_response(int fd, FILE *out, int *build_status, char *error,
    size_t error_len)
{
	char line[256];
	unsigned long len, left, chunk;
	char buf[8192], type[64];
	int status;

	for (;;) {
		if (read_line(fd, line, sizeof(line)) < 0) {
			set_error(error, error_len, "daemon stream interrupted before final status");
			fprintf(out, "qstar: %s\n", error);
			*build_status = 1;
			return 0;
		}
		if (sscanf(line, "final %d", &status) == 1) {
			*build_status = status;
			return 0;
		}
		if (sscanf(line, "event %63s %lu", type, &len) != 2 ||
		    len > QSTAR_DAEMON_MAX_EVENT) {
			set_error(error, error_len, "daemon stream has invalid event frame");
			fprintf(out, "qstar: %s\n", error);
			*build_status = 1;
			return 0;
		}
		left = len;
		while (left > 0) {
			chunk = left > sizeof(buf) ? (unsigned long)sizeof(buf) : left;
			if (read_exact(fd, buf, chunk) < 0) {
				set_error(error, error_len,
				    "daemon stream event body truncated");
				fprintf(out, "qstar: %s\n", error);
				*build_status = 1;
				return 0;
			}
			if (chunk > 0 && fwrite(buf, 1, chunk, out) != chunk) {
				set_error(error, error_len, "could not write daemon event");
				return -1;
			}
			left -= chunk;
		}
	}
}

/** daemon response body를 client stdout으로 복사하고 build status를 반환한다. */
static int
read_response(int fd, FILE *out, int *build_status, char *error, size_t error_len)
{
	char line[256];

	if (read_line(fd, line, sizeof(line)) < 0) {
		set_error(error, error_len, "daemon response missing");
		return -1;
	}
	if (strcmp(line, QSTAR_DAEMON_RESPONSE_MAGIC) == 0)
		return read_buffered_response(fd, out, build_status, error, error_len);
	if (strcmp(line, QSTAR_DAEMON_STREAM_MAGIC) == 0)
		return read_stream_response(fd, out, build_status, error, error_len);
	set_error(error, error_len, "daemon response has invalid magic");
	return -1;
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

/** read-only query request를 experimental daemon으로 보내고 JSON 응답을 out에 복사한다. */
static int
daemon_query_client(const char *socket_path, const char *method, const char *file,
    const char *cli_build_dir, const char *cli_profile, const char *cli_target,
    const char *cli_toolchain, const char *cli_stdlib, FILE *out)
{
	struct qstar_daemon_request req;
	char socket_buf[QSTAR_PATH_MAX], error[256];
	int fd, status;

	error[0] = '\0';
	if (!daemon_query_method_supported(method)) {
		fprintf(stderr, "qstar: unknown daemon query method '%s'\n", method);
		return 2;
	}
	if (!socket_path || !*socket_path) {
		if (default_socket_path(cli_build_dir, socket_buf, sizeof(socket_buf)) < 0) {
			fprintf(stderr, "qstar: could not compute default daemon socket path\n");
			return 1;
		}
		socket_path = socket_buf;
	}
	fd = connect_socket(socket_path, error, sizeof(error));
	if (fd < 0) {
		fprintf(out,
		    "{\"schema\":\"qstar-daemon-read-v1\",\"method\":");
		daemon_json_string(out, method);
		fputs(",\"status\":\"unavailable\",\"reason\":", out);
		daemon_json_string(out, error);
		fputs("}\n", out);
		return 1;
	}
	memset(&req, 0, sizeof(req));
	if (!getcwd(req.cwd, sizeof(req.cwd))) {
		fprintf(stderr, "qstar: getcwd: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (copy_string(req.file, sizeof(req.file), file) < 0 ||
	    copy_string(req.build_dir, sizeof(req.build_dir), cli_build_dir) < 0 ||
	    copy_string(req.profile, sizeof(req.profile), cli_profile) < 0 ||
	    copy_string(req.target, sizeof(req.target), cli_target) < 0 ||
	    copy_string(req.toolchain, sizeof(req.toolchain), cli_toolchain) < 0 ||
	    copy_string(req.stdlib_policy, sizeof(req.stdlib_policy), cli_stdlib) < 0) {
		fprintf(stderr, "qstar: daemon query field is too long\n");
		close(fd);
		return 1;
	}
	req.options.color_mode = QSTAR_COLOR_NEVER;
	if (send_query_request(fd, method, &req) < 0 ||
	    read_response(fd, out, &status, error, sizeof(error)) < 0) {
		fprintf(stderr, "qstar: daemon query failed: %s\n",
		    error[0] ? error : strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	return status;
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

/** watcher scope bitset을 trace용 이름으로 바꾼다. */
static const char *
watch_scope_name(int scope)
{
	if (scope & QSTAR_DAEMON_WATCH_AUTHORING)
		return "authoring";
	if (scope & QSTAR_DAEMON_WATCH_INPUT)
		return "input";
	return "unknown";
}

/** watcher scope에 맞는 conservative invalidation 이름을 반환한다. */
static const char *
watch_invalidation_name(int scope)
{
	return scope & QSTAR_DAEMON_WATCH_AUTHORING ? "graph" : "dirty-check";
}

/** watcher entry table과 backend fd를 닫는다. */
static void
watcher_close(struct qstar_daemon_watcher *watcher)
{
	size_t i;

	for (i = 0; i < watcher->len; i++) {
#if defined(__APPLE__) && defined(__MACH__)
		if (watcher->entries[i].fd >= 0)
			close(watcher->entries[i].fd);
#endif
		free(watcher->entries[i].path);
	}
	free(watcher->entries);
	watcher->entries = NULL;
	watcher->len = 0;
	watcher->cap = 0;
	if (watcher->active && watcher->backend_fd >= 0)
		close(watcher->backend_fd);
	watcher->active = 0;
	watcher->backend_fd = -1;
	watcher->backend[0] = '\0';
	watcher->incomplete = 0;
	watcher->skipped_missing = 0;
	watcher->graph_dirty = 0;
	watcher->input_dirty = 0;
}

/** watcher를 사용할 수 없을 때 fallback 이유를 저장한다. */
static void
watcher_unavailable(struct qstar_daemon_watcher *watcher, const char *reason)
{
	watcher_close(watcher);
	snprintf(watcher->backend, sizeof(watcher->backend), "%s", "polling");
	snprintf(watcher->reason, sizeof(watcher->reason), "%s",
	    reason && *reason ? reason : "unavailable");
}

/** watcher backend를 연다. 실패해도 daemon은 fingerprint fallback으로 계속 동작한다. */
static int
watcher_open(struct qstar_daemon_watcher *watcher)
{
	watcher_close(watcher);
#if defined(__APPLE__) && defined(__MACH__)
	watcher->backend_fd = kqueue();
	if (watcher->backend_fd < 0) {
		watcher_unavailable(watcher, strerror(errno));
		return 0;
	}
	watcher->active = 1;
	snprintf(watcher->backend, sizeof(watcher->backend), "%s", "kqueue");
	watcher->reason[0] = '\0';
	return 0;
#elif defined(__linux__)
	watcher->backend_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (watcher->backend_fd < 0) {
		watcher_unavailable(watcher, strerror(errno));
		return 0;
	}
	watcher->active = 1;
	snprintf(watcher->backend, sizeof(watcher->backend), "%s", "inotify");
	watcher->reason[0] = '\0';
	return 0;
#else
	watcher_unavailable(watcher, "unsupported-host");
	return 0;
#endif
}

/** watcher가 완전하지 않음을 기록하고 fingerprint scan fallback을 유지한다. */
static void
watcher_mark_incomplete(struct qstar_daemon_watcher *watcher, const char *reason)
{
	watcher->incomplete = 1;
	if (!watcher->reason[0])
		snprintf(watcher->reason, sizeof(watcher->reason), "%s",
		    reason && *reason ? reason : "incomplete");
}

/** watcher entry를 path로 찾는다. */
static struct qstar_daemon_watch_entry *
watcher_find_path(struct qstar_daemon_watcher *watcher, const char *path)
{
	size_t i;

	for (i = 0; i < watcher->len; i++) {
		if (strcmp(watcher->entries[i].path, path) == 0)
			return &watcher->entries[i];
	}
	return NULL;
}

#if defined(__APPLE__) && defined(__MACH__)
/** watcher entry를 kqueue ident fd로 찾는다. */
static struct qstar_daemon_watch_entry *
watcher_find_fd(struct qstar_daemon_watcher *watcher, int fd)
{
	size_t i;

	for (i = 0; i < watcher->len; i++) {
		if (watcher->entries[i].fd == fd)
			return &watcher->entries[i];
	}
	return NULL;
}
#elif defined(__linux__)
/** watcher entry를 inotify watch descriptor로 찾는다. */
static struct qstar_daemon_watch_entry *
watcher_find_wd(struct qstar_daemon_watcher *watcher, int wd)
{
	size_t i;

	for (i = 0; i < watcher->len; i++) {
		if (watcher->entries[i].wd == wd)
			return &watcher->entries[i];
	}
	return NULL;
}
#endif

/** watcher에 기존 파일 하나를 등록한다. 없는 파일은 다음 dirty check에 맡긴다. */
static int
watcher_add_existing(struct qstar_daemon_watcher *watcher, const char *path, int scope)
{
	struct qstar_daemon_watch_entry *entry, *items;
	struct stat st;
	size_t ncap;

	if (!watcher->active)
		return 0;
	entry = watcher_find_path(watcher, path);
	if (entry) {
		entry->scope |= scope;
		return 0;
	}
	if (stat(path, &st) < 0) {
		if (errno == ENOENT)
			watcher->skipped_missing = 1;
		else
			watcher_mark_incomplete(watcher, "stat-failed");
		return 0;
	}
	if (!S_ISREG(st.st_mode))
		return 0;
	if (watcher->len == watcher->cap) {
		ncap = watcher->cap ? watcher->cap * 2 : 64;
		items = realloc(watcher->entries, ncap * sizeof(items[0]));
		if (!items)
			return -1;
		watcher->entries = items;
		watcher->cap = ncap;
	}
	entry = &watcher->entries[watcher->len];
	memset(entry, 0, sizeof(*entry));
	entry->fd = -1;
	entry->wd = -1;
	entry->scope = scope;
	entry->path = qstar_strdup(path);
	if (!entry->path)
		return -1;
#if defined(__APPLE__) && defined(__MACH__)
	entry->fd = open(path, O_EVTONLY);
	if (entry->fd < 0 && O_EVTONLY != O_RDONLY)
		entry->fd = open(path, O_RDONLY);
	if (entry->fd < 0) {
		free(entry->path);
		entry->path = NULL;
		watcher_mark_incomplete(watcher, "watch-open-failed");
		return 0;
	}
	{
		struct kevent ev;

		EV_SET(&ev, entry->fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
		    NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE |
		    NOTE_RENAME | NOTE_REVOKE, 0, NULL);
		if (kevent(watcher->backend_fd, &ev, 1, NULL, 0, NULL) < 0) {
			close(entry->fd);
			free(entry->path);
			entry->path = NULL;
			watcher_mark_incomplete(watcher, "watch-add-failed");
			return 0;
		}
	}
#elif defined(__linux__)
	entry->wd = inotify_add_watch(watcher->backend_fd, path,
	    IN_CLOSE_WRITE | IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF |
	    IN_MOVE_SELF | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
	if (entry->wd < 0) {
		free(entry->path);
		entry->path = NULL;
		watcher_mark_incomplete(watcher, "watch-add-failed");
		return 0;
	}
#endif
	watcher->len++;
	return 0;
}

/** package-relative path 하나를 watcher에 등록한다. package 밖 path는 등록하지 않는다. */
static int
watcher_add_rel(struct qstar_daemon_server *server, const char *rel, int scope)
{
	char path[QSTAR_PATH_MAX];

	if (!rel || !*rel || !qstar_path_is_package_relative(rel))
		return 0;
	if (input_full_path(&server->graph, rel, path, sizeof(path)) < 0)
		return -1;
	return watcher_add_existing(&server->watcher, path, scope);
}

/** string list의 package-relative path들을 watcher에 등록한다. */
static int
watcher_add_list(struct qstar_daemon_server *server,
    const struct qstar_string_list *list, int scope)
{
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (watcher_add_rel(server, list->items[i], scope) < 0)
			return -1;
	}
	return 0;
}

/** watcher용 depfile token 하나를 package input watch로 추가한다. */
static int
watcher_add_depfile_token(struct qstar_daemon_server *server, char *token, size_t *len)
{
	if (*len == 0)
		return 0;
	token[*len] = '\0';
	*len = 0;
	return watcher_add_rel(server, token, QSTAR_DAEMON_WATCH_INPUT);
}

/** compiler depfile에서 discovered header를 best-effort로 watcher에 등록한다. */
static int
watcher_add_depfile_inputs(struct qstar_daemon_server *server, const char *depfile)
{
	char full[QSTAR_PATH_MAX], token[QSTAR_PATH_MAX];
	FILE *f;
	int c, rhs, escape;
	size_t len;

	if (!depfile || !*depfile || !qstar_path_is_package_relative(depfile))
		return 0;
	if (input_full_path(&server->graph, depfile, full, sizeof(full)) < 0)
		return -1;
	f = fopen(full, "r");
	if (!f) {
		if (errno == ENOENT)
			server->watcher.skipped_missing = 1;
		else
			watcher_mark_incomplete(&server->watcher, "depfile-open-failed");
		return 0;
	}
	rhs = 0;
	escape = 0;
	len = 0;
	while ((c = fgetc(f)) != EOF) {
		if (!rhs) {
			if (c == ':')
				rhs = 1;
			continue;
		}
		if (escape) {
			escape = 0;
			if (c == '\n' || c == '\r')
				continue;
		} else if (c == '\\') {
			escape = 1;
			continue;
		} else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			if (watcher_add_depfile_token(server, token, &len) < 0) {
				fclose(f);
				return -1;
			}
			continue;
		}
		if (len + 1 >= sizeof(token)) {
			watcher_mark_incomplete(&server->watcher, "depfile-token-too-long");
			len = 0;
			continue;
		}
		token[len++] = (char)c;
	}
	if (watcher_add_depfile_token(server, token, &len) < 0) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

/** 현재 Graph IR에서 daemon watcher가 관찰할 authoring/source/header path를 다시 구성한다. */
static int
watcher_refresh(struct qstar_daemon_server *server)
{
	size_t i;

	if (watcher_open(&server->watcher) < 0)
		return -1;
	if (!server->watcher.active)
		return 0;
	if (watcher_add_list(server, &server->graph.evaluated_fragments,
	    QSTAR_DAEMON_WATCH_AUTHORING) < 0)
		return -1;
	for (i = 0; i < server->graph.len; i++) {
		const struct qstar_target *target = &server->graph.targets[i];

		if (watcher_add_list(server, &target->sources,
		    QSTAR_DAEMON_WATCH_INPUT) < 0 ||
		    watcher_add_list(server, &target->public_headers,
		    QSTAR_DAEMON_WATCH_INPUT) < 0 ||
		    watcher_add_list(server, &target->private_headers,
		    QSTAR_DAEMON_WATCH_INPUT) < 0)
			return -1;
		if (watcher_add_rel(server, target->linker_script,
		    QSTAR_DAEMON_WATCH_INPUT) < 0 ||
		    watcher_add_rel(server, target->run_marker,
		    QSTAR_DAEMON_WATCH_INPUT) < 0 ||
		    watcher_add_rel(server, target->run_marker_log,
		    QSTAR_DAEMON_WATCH_INPUT) < 0)
			return -1;
	}
	for (i = 0; i < server->graph.genrule_len; i++) {
		const struct qstar_genrule *genrule = &server->graph.genrules[i];

		if (watcher_add_list(server, &genrule->inputs,
		    QSTAR_DAEMON_WATCH_INPUT) < 0 ||
		    watcher_add_list(server, &genrule->outputs,
		    QSTAR_DAEMON_WATCH_INPUT) < 0)
			return -1;
	}
	for (i = 0; i < server->graph.stage_len; i++) {
		if (watcher_add_list(server, &server->graph.stages[i].srcs,
		    QSTAR_DAEMON_WATCH_INPUT) < 0)
			return -1;
	}
	for (i = 0; i < server->graph.cached_action_len; i++) {
		const struct qstar_cached_action *action = &server->graph.cached_actions[i];

		if (watcher_add_list(server, &action->inputs,
		    QSTAR_DAEMON_WATCH_INPUT) < 0 ||
		    watcher_add_list(server, &action->depfile_inputs,
		    QSTAR_DAEMON_WATCH_INPUT) < 0 ||
		    watcher_add_list(server, &action->outputs,
		    QSTAR_DAEMON_WATCH_INPUT) < 0)
			return -1;
		if (watcher_add_rel(server, action->depfile,
		    QSTAR_DAEMON_WATCH_INPUT) < 0 ||
		    watcher_add_depfile_inputs(server, action->depfile) < 0)
			return -1;
	}
	return 0;
}

/** watcher event 하나를 conservative invalidation flag로 반영한다. */
static void
watcher_note_event(struct qstar_daemon_watcher *watcher,
    const struct qstar_daemon_watch_entry *entry, FILE *out, int trace,
    const char *reason)
{
	if (entry->scope & QSTAR_DAEMON_WATCH_AUTHORING)
		watcher->graph_dirty = 1;
	if (entry->scope & QSTAR_DAEMON_WATCH_INPUT)
		watcher->input_dirty = 1;
	if (trace)
		fprintf(out,
		    "daemon_watcher status=event backend=%s scope=%s path=%s invalidation=%s reason=%s\n",
		    watcher->backend, watch_scope_name(entry->scope), entry->path,
		    watch_invalidation_name(entry->scope),
		    reason && *reason ? reason : "changed");
}

/** watcher overflow나 backend 오류를 graph reload가 필요한 conservative 상태로 바꾼다. */
static void
watcher_note_uncertain(struct qstar_daemon_watcher *watcher, FILE *out, int trace,
    const char *reason)
{
	watcher->graph_dirty = 1;
	watcher_mark_incomplete(watcher, reason);
	if (trace)
		fprintf(out,
		    "daemon_watcher status=event backend=%s scope=unknown path=<unknown> invalidation=graph reason=%s\n",
		    watcher->backend[0] ? watcher->backend : "polling",
		    reason && *reason ? reason : "uncertain");
}

/** request 직전에 watcher event queue를 non-blocking으로 소비한다. */
static void
watcher_poll(struct qstar_daemon_watcher *watcher, FILE *out, int trace)
{
	if (!watcher->active)
		return;
	watcher->graph_dirty = 0;
	watcher->input_dirty = 0;
#if defined(__APPLE__) && defined(__MACH__)
	for (;;) {
		struct kevent events[32];
		struct timespec timeout;
		int n, i;

		timeout.tv_sec = 0;
		timeout.tv_nsec = 0;
		n = kevent(watcher->backend_fd, NULL, 0, events,
		    (int)(sizeof(events) / sizeof(events[0])), &timeout);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			watcher_note_uncertain(watcher, out, trace, strerror(errno));
			return;
		}
		if (n == 0)
			return;
		for (i = 0; i < n; i++) {
			struct qstar_daemon_watch_entry *entry;

			if (events[i].flags & EV_ERROR) {
				watcher_note_uncertain(watcher, out, trace, "event-error");
				continue;
			}
			entry = watcher_find_fd(watcher, (int)events[i].ident);
			if (!entry) {
				watcher_note_uncertain(watcher, out, trace, "unknown-watch");
				continue;
			}
			if (events[i].fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE))
				watcher_note_event(watcher, entry, out, trace, "delete-or-rename");
			else
				watcher_note_event(watcher, entry, out, trace, "changed");
		}
	}
#elif defined(__linux__)
	for (;;) {
		char buf[8192]
#if defined(__GNUC__)
		    __attribute__((aligned(__alignof__(struct inotify_event))))
#endif
		    ;
		ssize_t n;
		char *p, *end;

		n = read(watcher->backend_fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			watcher_note_uncertain(watcher, out, trace, strerror(errno));
			return;
		}
		if (n == 0)
			return;
		p = buf;
		end = buf + n;
		while (p + sizeof(struct inotify_event) <= end) {
			struct inotify_event *ev = (struct inotify_event *)p;
			struct qstar_daemon_watch_entry *entry;
			const char *reason;

			if (ev->mask & IN_Q_OVERFLOW) {
				watcher_note_uncertain(watcher, out, trace, "queue-overflow");
				p += sizeof(*ev) + ev->len;
				continue;
			}
			entry = watcher_find_wd(watcher, ev->wd);
			if (!entry) {
				watcher_note_uncertain(watcher, out, trace, "unknown-watch");
				p += sizeof(*ev) + ev->len;
				continue;
			}
			reason = ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_DELETE |
			    IN_MOVED_FROM | IN_MOVED_TO) ? "delete-or-rename" : "changed";
			watcher_note_event(watcher, entry, out, trace, reason);
			p += sizeof(*ev) + ev->len;
		}
	}
#else
	(void)out;
	(void)trace;
#endif
}

/** watcher 상태를 schedule trace에 기록한다. */
static void
watcher_trace_status(const struct qstar_daemon_watcher *watcher, FILE *out)
{
	if (!watcher->active) {
		fprintf(out, "daemon_watcher status=unavailable backend=polling reason=%s\n",
		    watcher->reason[0] ? watcher->reason : "unsupported-host");
		return;
	}
	fprintf(out,
	    "daemon_watcher status=active backend=%s watches=%zu incomplete=%d skipped_missing=%d\n",
	    watcher->backend, watcher->len, watcher->incomplete,
	    watcher->skipped_missing);
}

/** daemon server가 소유한 graph와 memory cache를 해제한다. */
static void
server_free(struct qstar_daemon_server *server)
{
	free_fps(server);
	watcher_close(&server->watcher);
	qstar_stella_state_cache_free(server->state_cache);
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

/** read API 표시용 absolute path를 현재 daemon workspace cwd 기준으로 만든다. */
static int
daemon_absolute_input_path(const struct qstar_graph *graph, const char *rel,
    char *dst, size_t dstlen)
{
	const char *root;
	char cwd[QSTAR_PATH_MAX];

	root = graph->package_root && *graph->package_root ? graph->package_root : ".";
	if (root[0] == '/')
		return snprintf(dst, dstlen, "%s/%s", root, rel) < (int)dstlen ? 0 : -1;
	if (!getcwd(cwd, sizeof(cwd)))
		return -1;
	if (strcmp(root, ".") == 0)
		return snprintf(dst, dstlen, "%s/%s", cwd, rel) < (int)dstlen ? 0 : -1;
	return snprintf(dst, dstlen, "%s/%s/%s", cwd, root, rel) < (int)dstlen ? 0 : -1;
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

/** read-only query가 build label 차이만으로 daemon memory graph를 밀어내지 않도록 비교한다. */
static int
same_graph_identity(const struct qstar_daemon_server *server,
    const struct qstar_daemon_request *req)
{
	return strcmp(server->cwd, req->cwd) == 0 &&
	    strcmp(server->file, req->file) == 0 &&
	    strcmp(server->build_dir, req->build_dir) == 0 &&
	    strcmp(server->profile, req->profile) == 0 &&
	    strcmp(server->target, req->target) == 0 &&
	    strcmp(server->toolchain, req->toolchain) == 0 &&
	    strcmp(server->stdlib_policy, req->stdlib_policy) == 0;
}

/** daemon memory에 Graph IR와 lowered plan cache 결과를 load한다. */
static int
load_graph(struct qstar_daemon_server *server, const struct qstar_daemon_request *req,
    FILE *out, const char **graph_status, const char **reason, const char *reload_reason)
{
	char cache_reason[128], store_reason[128];
	int rc, plan_loaded;

	*graph_status = "miss";
	*reason = "cold";
	watcher_close(&server->watcher);
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
	if (rc == 0 && watcher_refresh(server) < 0)
		rc = qstar_set_error(&server->graph,
		    "qstar: could not prepare daemon file watcher");
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
	snprintf(server->graph_reason, sizeof(server->graph_reason), "%s",
	    reload_reason && *reload_reason ? reload_reason :
	    (plan_loaded ? "plan-cache-hit" :
	    (cache_reason[0] ? cache_reason : "evaluated")));
	*reason = server->graph_reason;
	return 0;
}

/** request를 처리하기 전에 daemon memory graph hit/miss와 invalidation을 결정한다. */
static int
ensure_graph(struct qstar_daemon_server *server, const struct qstar_daemon_request *req,
    FILE *out, const char **graph_status, const char **reason, int trace)
{
	if (!server->graph_loaded)
		return load_graph(server, req, out, graph_status, reason, NULL);
	if (!same_identity(server, req)) {
		*reason = "identity-changed";
		return load_graph(server, req, out, graph_status, reason, "identity-changed");
	}
	watcher_poll(&server->watcher, out, trace);
	if (server->watcher.graph_dirty) {
		*reason = server->watcher.incomplete ? "watcher-uncertain" :
		    "watch-authoring-changed";
		return load_graph(server, req, out, graph_status, reason, *reason);
	}
	if ((!server->watcher.active || server->watcher.incomplete) &&
	    authoring_inputs_changed(server)) {
		*reason = "authoring-input-changed";
		return load_graph(server, req, out, graph_status, reason,
		    "authoring-input-changed");
	}
	*graph_status = "hit";
	*reason = "memory";
	return 0;
}

/** read-only query용 graph hit/miss를 결정하되 build label identity는 보존한다. */
static int
ensure_graph_for_query(struct qstar_daemon_server *server,
    const struct qstar_daemon_request *req, FILE *out, const char **graph_status,
    const char **reason)
{
	if (!server->graph_loaded)
		return load_graph(server, req, out, graph_status, reason, NULL);
	if (!same_graph_identity(server, req)) {
		*reason = "identity-changed";
		return load_graph(server, req, out, graph_status, reason, "identity-changed");
	}
	watcher_poll(&server->watcher, out, 0);
	if (server->watcher.graph_dirty) {
		*reason = server->watcher.incomplete ? "watcher-uncertain" :
		    "watch-authoring-changed";
		return load_graph(server, req, out, graph_status, reason, *reason);
	}
	if ((!server->watcher.active || server->watcher.incomplete) &&
	    authoring_inputs_changed(server)) {
		*reason = "authoring-input-changed";
		return load_graph(server, req, out, graph_status, reason,
		    "authoring-input-changed");
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

/** daemon stream final status frame을 보낸다. */
static int
send_stream_final(int fd, int status)
{
	char line[64];

	snprintf(line, sizeof(line), "final %d", status);
	return write_line(fd, line);
}

/** daemon stream에 text event를 간단히 보낸다. */
static int
send_stream_text(int fd, const char *type, const char *text)
{
	return write_event_frame(fd, type, text, strlen(text));
}

/** build request 하나를 Stella executor로 처리하고 stream response를 전송한다. */
static int
handle_build_request(struct qstar_daemon_server *server, int fd)
{
	struct qstar_daemon_request req;
	struct qstar_daemon_event_stream stream;
	char oldcwd[QSTAR_PATH_MAX];
	const char *graph_status, *reason;
	FILE *body;
	int rc;

	if (read_request(fd, &req) < 0) {
		if (write_line(fd, QSTAR_DAEMON_STREAM_MAGIC) < 0)
			return -1;
		(void)send_stream_text(fd, "diagnostic", "qstar: invalid daemon request\n");
		(void)send_stream_final(fd, 1);
		return -1;
	}
	if (write_line(fd, QSTAR_DAEMON_STREAM_MAGIC) < 0)
		return -1;
	body = open_event_stream(&stream, fd);
	if (!body) {
		(void)send_stream_text(fd, "diagnostic",
		    "qstar: daemon stream is unsupported on this host\n");
		(void)send_stream_final(fd, 1);
		return -1;
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
	if (ensure_graph(server, &req, body, &graph_status, &reason,
	    req.options.schedule_trace || req.options.verbose) < 0) {
		rc = 1;
		goto send_restore;
	}
	if (req.options.schedule_trace || req.options.verbose) {
		fprintf(body,
		    "daemon_server status=build graph=%s reason=%s experimental=1\n",
		    graph_status, reason);
		watcher_trace_status(&server->watcher, body);
	}
	if (!server->state_cache)
		server->state_cache = qstar_stella_state_cache_new();
	if (!server->state_cache) {
		fprintf(body, "qstar: daemon could not allocate state cache\n");
		rc = 1;
		goto send_restore;
	}
	rc = qstar_graph_build_with_state_cache(&server->graph,
	    req.label[0] ? req.label : NULL, &req.options, body,
	    server->state_cache) < 0 ? 1 : 0;
	if (rc == 0 && server->watcher.active && server->watcher.skipped_missing)
		(void)watcher_refresh(server);
	if (rc != 0 && server->graph.error[0])
		fprintf(body, "%s\n", server->graph.error);
send_restore:
	if (chdir(oldcwd) < 0 && rc == 0) {
		fprintf(body, "qstar: daemon could not restore cwd: %s\n", strerror(errno));
		rc = 1;
	}
send:
	if (finish_event_stream(&stream, body) < 0)
		rc = 1;
	fclose(body);
	if (send_stream_final(fd, rc) < 0)
		rc = 1;
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

/** daemon read API의 hello JSON을 출력한다. */
static void
emit_query_hello(FILE *out)
{
	fputs("{\"schema\":\"qstar-daemon-read-v1\",\"method\":\"hello\","
	    "\"status\":\"ok\",\"experimental\":true,\"readonly\":true,"
	    "\"methods\":[\"hello\",\"workspace.info\",\"targets.list\","
	    "\"diagnostics.list\",\"compile_commands.path\",\"build.summary\"]}\n", out);
}

/** graph load 실패를 daemon read API JSON error로 출력한다. */
static void
emit_query_graph_error(FILE *out, const char *method, const struct qstar_graph *graph)
{
	fputs("{\"schema\":\"qstar-daemon-read-v1\",\"method\":", out);
	daemon_json_string(out, method);
	fputs(",\"status\":\"error\",\"reason\":\"graph-load-failed\",\"message\":", out);
	daemon_json_string(out, graph->error[0] ? graph->error :
	    "qstar: daemon graph load failed");
	fputs("}\n", out);
}

/** workspace.info read API response를 출력한다. */
static void
emit_workspace_info(FILE *out, const struct qstar_daemon_request *req,
    const struct qstar_graph *graph, const char *graph_status, const char *reason)
{
	fputs("{\"schema\":\"qstar-daemon-read-v1\",\"method\":\"workspace.info\","
	    "\"status\":\"ok\",\"readonly\":true,\"graph\":{\"status\":", out);
	daemon_json_string(out, graph_status);
	fputs(",\"reason\":", out);
	daemon_json_string(out, reason);
	fputs("},\"request\":{\"cwd\":", out);
	daemon_json_string(out, req->cwd);
	fputs(",\"file\":", out);
	daemon_json_string(out, req->file);
	fputs(",\"build_dir_override\":", out);
	daemon_json_string(out, req->build_dir);
	fputs("},\"package_root\":", out);
	daemon_json_string(out, graph->package_root ? graph->package_root : ".");
	fputs(",\"project\":{\"name\":", out);
	daemon_json_string(out, graph->project.name && *graph->project.name ?
	    graph->project.name : "");
	fputs(",\"version\":", out);
	daemon_json_string(out, graph->project.version && *graph->project.version ?
	    graph->project.version : "");
	fputs(",\"root\":", out);
	daemon_json_string(out, graph->project.root && *graph->project.root ?
	    graph->project.root : ".");
	fputs(",\"build_dir\":", out);
	daemon_json_string(out, qstar_graph_build_dir(graph));
	fputs(",\"generated_dir\":", out);
	daemon_json_string(out, qstar_graph_generated_dir(graph));
	fputs(",\"compile_commands\":", out);
	daemon_json_string(out, qstar_graph_compile_commands_policy(graph));
	fputs(",\"generator\":", out);
	daemon_json_string(out, qstar_graph_generator(graph));
	fputs(",\"requested_generator\":", out);
	daemon_json_string(out, qstar_graph_requested_generator(graph));
	fprintf(out,
	    "},\"target_count\":%zu,\"config_count\":%zu,"
	    "\"generated_action_count\":%zu,\"stage_count\":%zu,"
	    "\"target_family_count\":%zu,\"diagnostic_count\":%zu}\n",
	    graph->len, graph->config_len, graph->genrule_len, graph->stage_len,
	    graph->family_len, graph->lint_len);
}

/** 현재 graph에 저장된 diagnostics를 side-effect 없이 JSON으로 출력한다. */
static void
emit_diagnostics_list(FILE *out, const struct qstar_graph *graph)
{
	size_t i;
	int errors, warnings, infos;

	errors = 0;
	warnings = 0;
	infos = 0;
	fputs("{\"schema\":\"qstar-daemon-read-v1\",\"method\":\"diagnostics.list\","
	    "\"status\":\"ok\",\"readonly\":true,\"diagnostics\":[", out);
	for (i = 0; i < graph->lint_len; i++) {
		const struct qstar_lint_diagnostic *diag = &graph->lint_diagnostics[i];

		if (i)
			fputc(',', out);
		if (diag->severity && strcmp(diag->severity, "error") == 0)
			errors++;
		else if (diag->severity && strcmp(diag->severity, "warning") == 0)
			warnings++;
		else
			infos++;
		fputs("{\"code\":", out);
		daemon_json_string(out, diag->code);
		fputs(",\"severity\":", out);
		daemon_json_string(out, diag->severity);
		fputs(",\"file\":", out);
		daemon_json_string(out, diag->file);
		fprintf(out, ",\"line\":%d,\"field\":", diag->line);
		daemon_json_string(out, diag->field);
		fputs(",\"label\":", out);
		daemon_json_string(out, diag->label);
		fputs(",\"message\":", out);
		daemon_json_string(out, diag->message);
		fputc('}', out);
	}
	fprintf(out, "],\"summary\":{\"errors\":%d,\"warnings\":%d,\"infos\":%d,"
	    "\"total\":%zu}}\n", errors, warnings, infos, graph->lint_len);
}

/** project policy에 따른 compile_commands.json path를 read API JSON으로 출력한다. */
static void
emit_compile_commands_path(FILE *out, const struct qstar_graph *graph)
{
	const char *policy;
	char rel[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX];
	int enabled;

	policy = qstar_graph_compile_commands_policy(graph);
	enabled = strcmp(policy, "off") != 0;
	rel[0] = '\0';
	full[0] = '\0';
	if (enabled) {
		if (strcmp(policy, "root") == 0) {
			snprintf(rel, sizeof(rel), "%s", "compile_commands.json");
		} else if (qstar_graph_build_path(graph, "compile_commands.json", rel,
		    sizeof(rel)) < 0 ||
		    daemon_absolute_input_path(graph, rel, full, sizeof(full)) < 0) {
			fputs("{\"schema\":\"qstar-daemon-read-v1\","
			    "\"method\":\"compile_commands.path\","
			    "\"status\":\"error\",\"reason\":\"path-too-long\"}\n", out);
			return;
		}
		if (strcmp(policy, "root") == 0 &&
		    daemon_absolute_input_path(graph, rel, full, sizeof(full)) < 0) {
			fputs("{\"schema\":\"qstar-daemon-read-v1\","
			    "\"method\":\"compile_commands.path\","
			    "\"status\":\"error\",\"reason\":\"path-too-long\"}\n", out);
			return;
		}
	}
	fputs("{\"schema\":\"qstar-daemon-read-v1\","
	    "\"method\":\"compile_commands.path\",\"status\":\"ok\",\"readonly\":true,"
	    "\"policy\":", out);
	daemon_json_string(out, policy);
	fprintf(out, ",\"enabled\":%s,\"path\":", enabled ? "true" : "false");
	daemon_json_string(out, enabled ? rel : "");
	fputs(",\"absolute_path\":", out);
	daemon_json_string(out, enabled ? full : "");
	fputs("}\n", out);
}

/** build summary 파일을 computed build dir 아래에서만 읽어 read API JSON으로 감싼다. */
static void
emit_build_summary(FILE *out, const struct qstar_graph *graph)
{
	char rel[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX];
	FILE *f;
	int c;

	if (qstar_graph_build_path(graph, "state/last-summary.json", rel,
	    sizeof(rel)) < 0 ||
	    daemon_absolute_input_path(graph, rel, full, sizeof(full)) < 0) {
		fputs("{\"schema\":\"qstar-daemon-read-v1\",\"method\":\"build.summary\","
		    "\"status\":\"error\",\"reason\":\"path-too-long\"}\n", out);
		return;
	}
	f = fopen(full, "rb");
	fputs("{\"schema\":\"qstar-daemon-read-v1\",\"method\":\"build.summary\","
	    "\"status\":\"ok\",\"readonly\":true,\"path\":", out);
	daemon_json_string(out, rel);
	fputs(",\"absolute_path\":", out);
	daemon_json_string(out, full);
	if (!f) {
		fputs(",\"exists\":false}\n", out);
		return;
	}
	fputs(",\"exists\":true,\"summary\":", out);
	while ((c = fgetc(f)) != EOF)
		fputc(c, out);
	fclose(f);
	fputs("}\n", out);
}

/** read-only query request 하나를 처리하고 JSON response를 전송한다. */
static int
handle_query_request(struct qstar_daemon_server *server, int fd)
{
	struct qstar_daemon_request req;
	char oldcwd[QSTAR_PATH_MAX], method[64];
	const char *graph_status, *reason;
	FILE *body, *scratch;
	int rc;

	body = NULL;
	scratch = NULL;
	rc = 0;
	if (read_query_request(fd, method, sizeof(method), &req) < 0)
		return send_simple_response(fd, 1,
		    "{\"schema\":\"qstar-daemon-read-v1\",\"status\":\"error\","
		    "\"reason\":\"invalid-request\"}\n");
	body = tmpfile();
	if (!body)
		return -1;
	if (!daemon_query_method_supported(method)) {
		fputs("{\"schema\":\"qstar-daemon-read-v1\",\"method\":", body);
		daemon_json_string(body, method);
		fputs(",\"status\":\"error\",\"reason\":\"unknown-method\"}\n", body);
		rc = send_file_response(fd, 2, body);
		fclose(body);
		return rc;
	}
	if (strcmp(method, "hello") == 0) {
		emit_query_hello(body);
		rc = send_file_response(fd, 0, body);
		fclose(body);
		return rc;
	}
	if (!getcwd(oldcwd, sizeof(oldcwd))) {
		fputs("{\"schema\":\"qstar-daemon-read-v1\",\"method\":", body);
		daemon_json_string(body, method);
		fprintf(body,
		    ",\"status\":\"error\",\"reason\":\"getcwd-failed\",\"message\":");
		daemon_json_string(body, strerror(errno));
		fputs("}\n", body);
		rc = send_file_response(fd, 1, body);
		fclose(body);
		return rc;
	}
	if (chdir(req.cwd) < 0) {
		fputs("{\"schema\":\"qstar-daemon-read-v1\",\"method\":", body);
		daemon_json_string(body, method);
		fputs(",\"status\":\"error\",\"reason\":\"workspace-enter-failed\","
		    "\"workspace\":", body);
		daemon_json_string(body, req.cwd);
		fputs(",\"message\":", body);
		daemon_json_string(body, strerror(errno));
		fputs("}\n", body);
		rc = send_file_response(fd, 1, body);
		fclose(body);
		return rc;
	}
	scratch = tmpfile();
	if (!scratch) {
		fputs("{\"schema\":\"qstar-daemon-read-v1\",\"method\":", body);
		daemon_json_string(body, method);
		fputs(",\"status\":\"error\",\"reason\":\"tmpfile-failed\"}\n", body);
		rc = 1;
		goto send_restore;
	}
	graph_status = "miss";
	reason = "cold";
	if (ensure_graph_for_query(server, &req, scratch, &graph_status, &reason) < 0) {
		emit_query_graph_error(body, method, &server->graph);
		rc = 1;
		goto send_restore;
	}
	if (strcmp(method, "workspace.info") == 0)
		emit_workspace_info(body, &req, &server->graph, graph_status, reason);
	else if (strcmp(method, "targets.list") == 0) {
		if (qstar_graph_list_targets_json(&server->graph, body) < 0) {
			fputs("{\"schema\":\"qstar-daemon-read-v1\","
			    "\"method\":\"targets.list\",\"status\":\"error\","
			    "\"reason\":\"out-of-memory\"}\n", body);
			rc = 1;
		}
	} else if (strcmp(method, "diagnostics.list") == 0) {
		emit_diagnostics_list(body, &server->graph);
	} else if (strcmp(method, "compile_commands.path") == 0) {
		emit_compile_commands_path(body, &server->graph);
	} else if (strcmp(method, "build.summary") == 0) {
		emit_build_summary(body, &server->graph);
	}
send_restore:
	if (scratch)
		fclose(scratch);
	if (chdir(oldcwd) < 0 && rc == 0) {
		fputs("{\"schema\":\"qstar-daemon-read-v1\",\"method\":", body);
		daemon_json_string(body, method);
		fputs(",\"status\":\"error\",\"reason\":\"cwd-restore-failed\","
		    "\"message\":", body);
		daemon_json_string(body, strerror(errno));
		fputs("}\n", body);
		rc = 1;
	}
	if (send_file_response(fd, rc == 0 ? 0 : 1, body) < 0)
		rc = 1;
	fclose(body);
	return rc == 0 ? 0 : -1;
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
			else if (strcmp(magic, QSTAR_DAEMON_QUERY_MAGIC) == 0)
				(void)handle_query_request(&server, client_fd);
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
	fputs("       qstar [options] daemon --socket path --query method\n", out);
	fputs("methods: hello, workspace.info, targets.list, diagnostics.list, compile_commands.path, build.summary\n", out);
	fputs("Experimental Stella daemon. The stable build path is unchanged.\n", out);
}

/** experimental persistent Stella daemon command를 실행한다. */
int
qstar_daemon_command(int argc, char **argv, const char *file,
    const char *cli_build_dir, const char *cli_profile, const char *cli_target,
    const char *cli_toolchain, const char *cli_stdlib, FILE *out)
{
	const char *socket_path, *query_method;
	char socket_buf[QSTAR_PATH_MAX];
	int serve, status, i;

	socket_path = NULL;
	query_method = NULL;
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
		} else if (strcmp(argv[i], "--query") == 0 ||
		    strcmp(argv[i], "query") == 0) {
			if (i + 1 >= argc) {
				daemon_usage(stderr);
				return 2;
			}
			query_method = argv[++i];
		} else if (strcmp(argv[i], "--help") == 0 ||
		    strcmp(argv[i], "-h") == 0) {
			daemon_usage(out);
			return 0;
		} else {
			daemon_usage(stderr);
			return 2;
		}
	}
	if (serve + status + (query_method ? 1 : 0) != 1) {
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
	if (query_method)
		return daemon_query_client(socket_path, query_method, file, cli_build_dir,
		    cli_profile, cli_target, cli_toolchain, cli_stdlib, out);
	return daemon_status(socket_path, out);
}
