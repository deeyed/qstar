#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define QSTAR_PLATFORM_WINDOWS 1
#else
#define QSTAR_PLATFORM_WINDOWS 0
#endif

#if QSTAR_PLATFORM_WINDOWS
#if defined(__has_include)
#if __has_include(<windows.h>)
#define QSTAR_HAVE_WINDOWS_API 1
#endif
#endif
#endif

#ifndef QSTAR_HAVE_WINDOWS_API
#define QSTAR_HAVE_WINDOWS_API 0
#endif

#if QSTAR_HAVE_WINDOWS_API
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#if !QSTAR_PLATFORM_WINDOWS
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if !QSTAR_PLATFORM_WINDOWS && (defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__)))
#define QSTAR_HAVE_POSIX_SPAWN_RUNNER 1
#else
#define QSTAR_HAVE_POSIX_SPAWN_RUNNER 0
#endif

#if QSTAR_PLATFORM_WINDOWS && QSTAR_HAVE_WINDOWS_API
#elif QSTAR_PLATFORM_WINDOWS && defined(_MSC_VER)
extern char **_environ;
#define qstar_platform_environ _environ
#elif QSTAR_PLATFORM_WINDOWS
extern char **environ;
#define qstar_platform_environ environ
#else
extern char **environ;
#define qstar_platform_environ environ
#endif

enum qstar_process_stdio_mode {
	QSTAR_PROCESS_STDIO_INHERIT,
	QSTAR_PROCESS_STDIO_CAPTURE,
	QSTAR_PROCESS_STDIO_FILES
};

struct qstar_process_start_request {
	const char *cwd;
	char *const *argv;
	enum qstar_process_stdio_mode stdio_mode;
	int stdout_read_fd;
	int stdout_write_fd;
	int stderr_read_fd;
	int stderr_write_fd;
	const char *stdout_path;
	const char *stderr_path;
};

static size_t
argv_count(char *const *argv)
{
	size_t n = 0;

	while (argv && argv[n])
		n++;
	return n;
}

static int
windows_argv_needs_sh_adapter(char *const argv[])
{
#if QSTAR_PLATFORM_WINDOWS
	size_t ns, n;

	if (!argv || !argv[0])
		return 0;
	ns = strlen(argv[0]);
	n = 3;
	if (ns < n)
		return 0;
	return strcmp(argv[0] + ns - n, ".sh") == 0;
#else
	(void)argv;
	return 0;
#endif
}

static char **
windows_effective_argv(char *const argv[], size_t *argc_out)
{
	size_t argc, i;
	char **effective;

	argc = argv_count(argv);
	if (argc_out)
		*argc_out = argc;
	if (!windows_argv_needs_sh_adapter(argv))
		return NULL;
	effective = calloc(argc + 2, sizeof(effective[0]));
	if (!effective)
		return NULL;
	effective[0] = (char *)"sh";
	for (i = 0; i < argc; i++)
		effective[i + 1] = argv[i];
	effective[argc + 1] = NULL;
	if (argc_out)
		*argc_out = argc + 1;
	return effective;
}

#if !QSTAR_PLATFORM_WINDOWS
/** fd를 nonblocking mode로 전환한다. */
static int
set_nonblocking_fd(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
#endif

/** platform별 child stdout/stderr pipe 한 쌍을 준비한다. */
int
qstar_platform_pipe_open(int *read_fd, int *write_fd)
{
#if QSTAR_PLATFORM_WINDOWS
#if QSTAR_HAVE_WINDOWS_API
	SECURITY_ATTRIBUTES sa;
	HANDLE read_handle = INVALID_HANDLE_VALUE;
	HANDLE write_handle = INVALID_HANDLE_VALUE;
	int rfd = -1, wfd = -1;

	if (!read_fd || !write_fd)
		return -1;
	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	if (!CreatePipe(&read_handle, &write_handle, &sa, 0))
		return -1;
	if (!SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0))
		goto fail;
	rfd = _open_osfhandle((intptr_t)read_handle, _O_RDONLY | _O_BINARY);
	if (rfd < 0)
		goto fail;
	read_handle = INVALID_HANDLE_VALUE;
	wfd = _open_osfhandle((intptr_t)write_handle, _O_WRONLY | _O_BINARY);
	if (wfd < 0)
		goto fail;
	write_handle = INVALID_HANDLE_VALUE;
	*read_fd = rfd;
	*write_fd = wfd;
	return 0;
fail:
	if (rfd >= 0)
		_close(rfd);
	if (wfd >= 0)
		_close(wfd);
	if (read_handle != INVALID_HANDLE_VALUE)
		CloseHandle(read_handle);
	if (write_handle != INVALID_HANDLE_VALUE)
		CloseHandle(write_handle);
	return -1;
#else
	if (read_fd)
		*read_fd = -1;
	if (write_fd)
		*write_fd = -1;
	return 0;
#endif
#else
	int fds[2];

	if (!read_fd || !write_fd)
		return -1;
	if (pipe(fds) < 0)
		return -1;
	if (set_nonblocking_fd(fds[0]) < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	*read_fd = fds[0];
	*write_fd = fds[1];
	return 0;
#endif
}

/** platform fd close primitive다. */
void
qstar_platform_process_close_fd(int *fd)
{
	if (!fd || *fd < 0)
		return;
#if QSTAR_PLATFORM_WINDOWS
#if QSTAR_HAVE_WINDOWS_API
	_close(*fd);
#endif
#else
	close(*fd);
#endif
	*fd = -1;
}

static int
append_char(char **cursor, size_t *remaining, char c)
{
	if (*remaining <= 1)
		return -1;
	**cursor = c;
	(*cursor)++;
	(*remaining)--;
	**cursor = '\0';
	return 0;
}

static int
append_repeated(char **cursor, size_t *remaining, char c, size_t count)
{
	while (count-- > 0) {
		if (append_char(cursor, remaining, c) < 0)
			return -1;
	}
	return 0;
}

static int
append_string(char **cursor, size_t *remaining, const char *s)
{
	while (*s) {
		if (append_char(cursor, remaining, *s++) < 0)
			return -1;
	}
	return 0;
}

static int
windows_arg_needs_quotes(const char *arg)
{
	if (!arg || !*arg)
		return 1;
	for (; *arg; arg++) {
		if (*arg == ' ' || *arg == '\t' || *arg == '"')
			return 1;
	}
	return 0;
}

static int
append_windows_quoted_arg(char **cursor, size_t *remaining, const char *arg)
{
	size_t slashes = 0;
	int quote;

	arg = arg ? arg : "";
	quote = windows_arg_needs_quotes(arg);
	if (!quote)
		return append_string(cursor, remaining, arg);
	if (append_char(cursor, remaining, '"') < 0)
		return -1;
	for (; *arg; arg++) {
		if (*arg == '\\') {
			slashes++;
			continue;
		}
		if (*arg == '"') {
			if (append_repeated(cursor, remaining, '\\', slashes * 2 + 1) < 0 ||
			    append_char(cursor, remaining, '"') < 0)
				return -1;
			slashes = 0;
			continue;
		}
		if (append_repeated(cursor, remaining, '\\', slashes) < 0 ||
		    append_char(cursor, remaining, *arg) < 0)
			return -1;
		slashes = 0;
	}
	if (append_repeated(cursor, remaining, '\\', slashes * 2) < 0 ||
	    append_char(cursor, remaining, '"') < 0)
		return -1;
	return 0;
}

/** Windows CreateProcess command line quoting을 argv vector에서 만든다. */
int
qstar_platform_windows_command_line_from_argv(char *const argv[], char *dst, size_t dstlen)
{
	char *cursor;
	char **effective = NULL;
	char *const *items;
	size_t remaining, i;

	if (!argv || !argv[0] || !dst || dstlen == 0)
		return -1;
	effective = windows_effective_argv(argv, NULL);
	if (windows_argv_needs_sh_adapter(argv) && !effective)
		return -1;
	items = effective ? (char *const *)effective : (char *const *)argv;
	cursor = dst;
	remaining = dstlen;
	*cursor = '\0';
	for (i = 0; items[i]; i++) {
		if (i > 0 && append_char(&cursor, &remaining, ' ') < 0)
			goto fail;
		if (append_windows_quoted_arg(&cursor, &remaining, items[i]) < 0)
			goto fail;
	}
	free(effective);
	return 0;
fail:
	free(effective);
	return -1;
}

/** 현재 process environment를 Windows double-NUL env block으로 직렬화한다. */
int
qstar_platform_windows_env_block_from_current(char *dst, size_t dstlen, size_t *needed_out)
{
#if QSTAR_PLATFORM_WINDOWS && QSTAR_HAVE_WINDOWS_API
	LPCH block, p;
	size_t needed = 1;

	block = GetEnvironmentStringsA();
	if (!block)
		return -1;
	for (p = block; *p; p += strlen(p) + 1)
		needed += strlen(p) + 1;
	if (needed_out)
		*needed_out = needed;
	if (dst) {
		if (dstlen < needed) {
			FreeEnvironmentStringsA(block);
			return -1;
		}
		memcpy(dst, block, needed);
	}
	FreeEnvironmentStringsA(block);
	return 0;
#else
	char **envp;
	size_t needed = 1;
	char *cursor;

	for (envp = qstar_platform_environ; envp && *envp; envp++)
		needed += strlen(*envp) + 1;
	if (needed_out)
		*needed_out = needed;
	if (!dst)
		return 0;
	if (dstlen < needed)
		return -1;
	cursor = dst;
	for (envp = qstar_platform_environ; envp && *envp; envp++) {
		size_t len = strlen(*envp);
		memcpy(cursor, *envp, len);
		cursor += len;
		*cursor++ = '\0';
	}
	*cursor = '\0';
	return 0;
#endif
}

#if QSTAR_PLATFORM_WINDOWS
static int
windows_normalize_cwd(const char *cwd, char *dst, size_t dstlen)
{
#if QSTAR_HAVE_WINDOWS_API
	char adjusted[QSTAR_PATH_MAX];
	DWORD n;
	size_t i;

	cwd = cwd && *cwd ? cwd : ".";
	if (cwd[0] == '/' && cwd[1] && cwd[2] == '/') {
		if (snprintf(adjusted, sizeof(adjusted), "%c:/%s", cwd[1], cwd + 3) >=
		    (int)sizeof(adjusted))
			return -1;
	} else {
		if (snprintf(adjusted, sizeof(adjusted), "%s", cwd) >=
		    (int)sizeof(adjusted))
			return -1;
	}
	for (i = 0; adjusted[i]; i++) {
		if (adjusted[i] == '/')
			adjusted[i] = '\\';
	}
	n = GetFullPathNameA(adjusted, (DWORD)dstlen, dst, NULL);
	return n > 0 && n < dstlen ? 0 : -1;
#else
	cwd = cwd && *cwd ? cwd : ".";
	return snprintf(dst, dstlen, "%s", cwd) < (int)dstlen ? 0 : -1;
#endif
}

#if QSTAR_HAVE_WINDOWS_API
static HANDLE
windows_fd_handle(int fd)
{
	intptr_t raw;

	if (fd < 0)
		return INVALID_HANDLE_VALUE;
	raw = _get_osfhandle(fd);
	return raw == -1 ? INVALID_HANDLE_VALUE : (HANDLE)raw;
}

static HANDLE
windows_open_inherited_output_file(const char *path)
{
	SECURITY_ATTRIBUTES sa;

	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	return CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
	    FILE_ATTRIBUTE_NORMAL, NULL);
}
#endif

static int
windows_start_process(struct qstar_graph *graph,
    const struct qstar_process_start_request *req, qstar_process_id *pid_out,
    const char **runner_out)
{
#if QSTAR_HAVE_WINDOWS_API
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	HANDLE stdout_handle = INVALID_HANDLE_VALUE;
	HANDLE stderr_handle = INVALID_HANDLE_VALUE;
	char command_line[32768];
	char cwd[QSTAR_PATH_MAX];
	char *env_block = NULL;
	size_t env_bytes = 0;

	if (pid_out)
		*pid_out = 0;
	if (!req || !req->argv || !req->argv[0])
		return qstar_set_error(graph, "qstar: process argv is empty");
	if (runner_out)
		*runner_out = windows_argv_needs_sh_adapter((char *const *)req->argv) ?
		    "createprocess-sh" : "createprocess";
	if (qstar_platform_windows_command_line_from_argv((char *const *)req->argv,
	    command_line, sizeof(command_line)) < 0)
		return qstar_set_error(graph,
		    "qstar: Windows process command line is too long");
	if (windows_normalize_cwd(req->cwd, cwd, sizeof(cwd)) < 0)
		return qstar_set_error(graph, "qstar: Windows process cwd is too long");
	if (qstar_platform_windows_env_block_from_current(NULL, 0, &env_bytes) < 0)
		return qstar_set_error(graph,
		    "qstar: could not size Windows process environment block");
	env_block = malloc(env_bytes ? env_bytes : 1);
	if (!env_block)
		return qstar_set_error(graph, "qstar: out of memory");
	if (qstar_platform_windows_env_block_from_current(env_block, env_bytes, NULL) < 0) {
		free(env_block);
		return qstar_set_error(graph,
		    "qstar: could not build Windows process environment block");
	}
	memset(&si, 0, sizeof(si));
	memset(&pi, 0, sizeof(pi));
	si.cb = sizeof(si);
	if (req->stdio_mode == QSTAR_PROCESS_STDIO_CAPTURE) {
		stdout_handle = windows_fd_handle(req->stdout_write_fd);
		stderr_handle = windows_fd_handle(req->stderr_write_fd);
	} else if (req->stdio_mode == QSTAR_PROCESS_STDIO_FILES) {
		stdout_handle = windows_open_inherited_output_file(req->stdout_path);
		stderr_handle = windows_open_inherited_output_file(req->stderr_path);
	} else {
		stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
		stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
	}
	if (stdout_handle == INVALID_HANDLE_VALUE || stderr_handle == INVALID_HANDLE_VALUE) {
		if (req->stdio_mode == QSTAR_PROCESS_STDIO_FILES) {
			if (stdout_handle != INVALID_HANDLE_VALUE)
				CloseHandle(stdout_handle);
			if (stderr_handle != INVALID_HANDLE_VALUE)
				CloseHandle(stderr_handle);
		}
		free(env_block);
		return qstar_set_error(graph,
		    "qstar: could not prepare Windows process stdio handles");
	}
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = stdout_handle;
	si.hStdError = stderr_handle;
	if (!CreateProcessA(NULL, command_line, NULL, NULL, TRUE, 0, env_block, cwd,
	    &si, &pi)) {
		DWORD err = GetLastError();
		if (req->stdio_mode == QSTAR_PROCESS_STDIO_FILES) {
			CloseHandle(stdout_handle);
			CloseHandle(stderr_handle);
		}
		free(env_block);
		return qstar_set_error(graph,
		    "qstar: CreateProcess failed for '%s' (winerr=%lu)",
		    req->argv[0], (unsigned long)err);
	}
	if (req->stdio_mode == QSTAR_PROCESS_STDIO_FILES) {
		CloseHandle(stdout_handle);
		CloseHandle(stderr_handle);
	}
	CloseHandle(pi.hThread);
	free(env_block);
	*pid_out = (qstar_process_id)(intptr_t)pi.hProcess;
	return 0;
#else
	(void)req;
	if (pid_out)
		*pid_out = 0;
	if (runner_out)
		*runner_out = "createprocess-unavailable";
	return qstar_set_error(graph,
	    "qstar: Windows CreateProcess API is unavailable in this build");
#endif
}
#else
#if QSTAR_HAVE_POSIX_SPAWN_RUNNER
/** 현재 platform의 posix_spawn cwd file action을 추가한다. */
static int
spawn_actions_addchdir(posix_spawn_file_actions_t *actions, const char *path)
{
#if defined(__APPLE__)
	return posix_spawn_file_actions_addchdir(actions, path);
#elif defined(__linux__) && defined(__GLIBC__)
	return posix_spawn_file_actions_addchdir_np(actions, path);
#else
	(void)actions;
	(void)path;
	errno = ENOSYS;
	return -1;
#endif
}

static int
posix_spawn_process(const struct qstar_process_start_request *req,
    qstar_process_id *pid_out)
{
	posix_spawn_file_actions_t actions;
	pid_t pid;
	int rc;

	rc = posix_spawn_file_actions_init(&actions);
	if (rc != 0)
		return -1;
	if (spawn_actions_addchdir(&actions, req->cwd ? req->cwd : ".") != 0)
		goto fail;
	if (req->stdio_mode == QSTAR_PROCESS_STDIO_CAPTURE) {
		if ((req->stdout_read_fd >= 0 &&
		    posix_spawn_file_actions_addclose(&actions, req->stdout_read_fd) != 0) ||
		    (req->stderr_read_fd >= 0 &&
		    posix_spawn_file_actions_addclose(&actions, req->stderr_read_fd) != 0) ||
		    posix_spawn_file_actions_adddup2(&actions, req->stdout_write_fd,
		    STDOUT_FILENO) != 0 ||
		    posix_spawn_file_actions_adddup2(&actions, req->stderr_write_fd,
		    STDERR_FILENO) != 0 ||
		    posix_spawn_file_actions_addclose(&actions, req->stdout_write_fd) != 0 ||
		    posix_spawn_file_actions_addclose(&actions, req->stderr_write_fd) != 0)
			goto fail;
	} else if (req->stdio_mode == QSTAR_PROCESS_STDIO_FILES) {
		if (posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
		    req->stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0666) != 0 ||
		    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
		    req->stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0666) != 0)
			goto fail;
	}
	rc = posix_spawnp(&pid, req->argv[0], &actions, NULL, req->argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	if (rc != 0)
		return -1;
	*pid_out = (qstar_process_id)pid;
	return 0;
fail:
	posix_spawn_file_actions_destroy(&actions);
	return -1;
}
#endif

static void
child_redirect_or_exit(const struct qstar_process_start_request *req)
{
	int fdout, fderr;

	if (req->stdio_mode == QSTAR_PROCESS_STDIO_CAPTURE) {
		if (req->stdout_read_fd >= 0)
			close(req->stdout_read_fd);
		if (req->stderr_read_fd >= 0)
			close(req->stderr_read_fd);
		if (req->stdout_write_fd < 0 || req->stderr_write_fd < 0 ||
		    dup2(req->stdout_write_fd, STDOUT_FILENO) < 0 ||
		    dup2(req->stderr_write_fd, STDERR_FILENO) < 0)
			_exit(127);
		close(req->stdout_write_fd);
		close(req->stderr_write_fd);
	} else if (req->stdio_mode == QSTAR_PROCESS_STDIO_FILES) {
		fdout = open(req->stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		fderr = open(req->stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fdout < 0 || fderr < 0 || dup2(fdout, STDOUT_FILENO) < 0 ||
		    dup2(fderr, STDERR_FILENO) < 0)
			_exit(127);
		close(fdout);
		close(fderr);
	}
}

static int
fork_process(const struct qstar_process_start_request *req, qstar_process_id *pid_out)
{
	pid_t pid;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		if (chdir(req->cwd ? req->cwd : ".") < 0)
			_exit(127);
		child_redirect_or_exit(req);
		execvp(req->argv[0], req->argv);
		_exit(127);
	}
	*pid_out = (qstar_process_id)pid;
	return 0;
}

static int
posix_start_process(struct qstar_graph *graph, const struct qstar_process_start_request *req,
    qstar_process_id *pid_out, const char **runner_out)
{
	if (!req || !req->argv || !req->argv[0])
		return qstar_set_error(graph, "qstar: process argv is empty");
#if QSTAR_HAVE_POSIX_SPAWN_RUNNER
	if (posix_spawn_process(req, pid_out) == 0) {
		if (runner_out)
			*runner_out = "posix_spawn";
		return 0;
	}
#endif
	if (fork_process(req, pid_out) < 0)
		return qstar_set_error(graph, "qstar: fork failed");
	if (runner_out)
		*runner_out = "fork";
	return 0;
}
#endif

static int
start_process(struct qstar_graph *graph, const struct qstar_process_start_request *req,
    qstar_process_id *pid_out, const char **runner_out)
{
#if QSTAR_PLATFORM_WINDOWS
	return windows_start_process(graph, req, pid_out, runner_out);
#else
	return posix_start_process(graph, req, pid_out, runner_out);
#endif
}

/** stdout/stderr capture pipe를 연결해 child process를 시작한다. */
int
qstar_platform_process_start_captured(struct qstar_graph *graph, const char *cwd,
    char *const argv[], int stdout_read_fd, int stdout_write_fd, int stderr_read_fd,
    int stderr_write_fd, qstar_process_id *pid_out, const char **runner_out)
{
	struct qstar_process_start_request req;

	memset(&req, 0, sizeof(req));
	req.cwd = cwd;
	req.argv = argv;
	req.stdio_mode = QSTAR_PROCESS_STDIO_CAPTURE;
	req.stdout_read_fd = stdout_read_fd;
	req.stdout_write_fd = stdout_write_fd;
	req.stderr_read_fd = stderr_read_fd;
	req.stderr_write_fd = stderr_write_fd;
	return start_process(graph, &req, pid_out, runner_out);
}

/** parent stdout/stderr를 상속해 child process를 시작한다. */
int
qstar_platform_process_start_inherit(struct qstar_graph *graph, const char *cwd,
    char *const argv[], qstar_process_id *pid_out, const char **runner_out)
{
	struct qstar_process_start_request req;

	memset(&req, 0, sizeof(req));
	req.cwd = cwd;
	req.argv = argv;
	req.stdio_mode = QSTAR_PROCESS_STDIO_INHERIT;
	return start_process(graph, &req, pid_out, runner_out);
}

/** stdout/stderr를 지정 파일로 redirect해 child process를 시작한다. */
int
qstar_platform_process_start_file_output(struct qstar_graph *graph, const char *cwd,
    char *const argv[], const char *stdout_path, const char *stderr_path,
    qstar_process_id *pid_out, const char **runner_out)
{
	struct qstar_process_start_request req;

	memset(&req, 0, sizeof(req));
	req.cwd = cwd;
	req.argv = argv;
	req.stdio_mode = QSTAR_PROCESS_STDIO_FILES;
	req.stdout_path = stdout_path;
	req.stderr_path = stderr_path;
	return start_process(graph, &req, pid_out, runner_out);
}

/** child process가 끝났는지 non-blocking 방식으로 확인한다. */
int
qstar_platform_process_wait_nohang(qstar_process_id pid, int *status, int *done)
{
#if QSTAR_PLATFORM_WINDOWS
#if QSTAR_HAVE_WINDOWS_API
	HANDLE process = (HANDLE)(intptr_t)pid;
	DWORD wait_rc, code;

	if (!process)
		return -1;
	wait_rc = WaitForSingleObject(process, 0);
	if (wait_rc == WAIT_TIMEOUT) {
		if (done)
			*done = 0;
		return 0;
	}
	if (wait_rc != WAIT_OBJECT_0)
		return -1;
	if (!GetExitCodeProcess(process, &code))
		code = 127;
	if (status)
		*status = (int)code;
	if (done)
		*done = 1;
	CloseHandle(process);
	return 0;
#else
	(void)pid;
	if (status)
		*status = 127;
	if (done)
		*done = 1;
	return -1;
#endif
#else
	pid_t waited, posix_pid = (pid_t)pid;

	for (;;) {
		waited = waitpid(posix_pid, status, WNOHANG);
		if (waited == posix_pid) {
			*done = 1;
			return 0;
		}
		if (waited == 0) {
			*done = 0;
			return 0;
		}
		if (errno == EINTR)
			continue;
		return -1;
	}
#endif
}

/** child process 종료를 blocking으로 기다린다. */
int
qstar_platform_process_wait_blocking(qstar_process_id pid, int *status)
{
#if QSTAR_PLATFORM_WINDOWS
#if QSTAR_HAVE_WINDOWS_API
	HANDLE process = (HANDLE)(intptr_t)pid;
	DWORD code;

	if (!process)
		return -1;
	if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) {
		CloseHandle(process);
		return -1;
	}
	if (!GetExitCodeProcess(process, &code))
		code = 127;
	if (status)
		*status = (int)code;
	CloseHandle(process);
	return 0;
#else
	(void)pid;
	if (status)
		*status = 127;
	return -1;
#endif
#else
	pid_t waited, posix_pid = (pid_t)pid;

	for (;;) {
		waited = waitpid(posix_pid, status, 0);
		if (waited == posix_pid)
			return 0;
		if (waited < 0 && errno == EINTR)
			continue;
		return -1;
	}
#endif
}

/** timeout/cancel 시 child process를 종료한다. */
void
qstar_platform_process_terminate(qstar_process_id pid, int *status)
{
#if QSTAR_PLATFORM_WINDOWS
#if QSTAR_HAVE_WINDOWS_API
	HANDLE process = (HANDLE)(intptr_t)pid;
	DWORD code;

	if (!process) {
		if (status)
			*status = 124;
		return;
	}
	TerminateProcess(process, 124);
	WaitForSingleObject(process, INFINITE);
	if (!GetExitCodeProcess(process, &code))
		code = 124;
	if (status)
		*status = (int)code;
	CloseHandle(process);
#else
	(void)pid;
	if (status)
		*status = 124;
#endif
#else
	kill((pid_t)pid, SIGKILL);
	(void)qstar_platform_process_wait_blocking(pid, status);
#endif
}

/** platform별 process status를 QStar exit code로 정규화한다. */
int
qstar_platform_process_exit_code(int status)
{
#if QSTAR_PLATFORM_WINDOWS
	return status;
#else
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}

/** process status가 정상 종료 0인지 확인한다. */
int
qstar_platform_process_exited_success(int status)
{
#if QSTAR_PLATFORM_WINDOWS
	return status == 0;
#else
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

/** process status가 exit code를 포함하는지 확인한다. */
int
qstar_platform_process_has_exit_code(int status)
{
#if QSTAR_PLATFORM_WINDOWS
	(void)status;
	return 1;
#else
	return WIFEXITED(status);
#endif
}

/** process status의 exit code를 가져온다. */
int
qstar_platform_process_status_exit_code(int status)
{
#if QSTAR_PLATFORM_WINDOWS
	return status;
#else
	return WEXITSTATUS(status);
#endif
}

/** POSIX signal 종료 상태를 diagnostic용으로 반환한다. */
int
qstar_platform_process_signal_number(int status)
{
#if QSTAR_PLATFORM_WINDOWS
	(void)status;
	return 0;
#else
	return WTERMSIG(status);
#endif
}

/** process output/event wait primitive다. */
int
qstar_platform_process_poll(struct qstar_platform_pollfd *fds,
    qstar_platform_poll_count nfds, int timeout_ms)
{
#if QSTAR_PLATFORM_WINDOWS
	(void)fds;
#if QSTAR_HAVE_WINDOWS_API
	if (timeout_ms > 0)
		Sleep((DWORD)timeout_ms);
#else
	(void)timeout_ms;
#endif
	return nfds > 0 ? 1 : 0;
#else
	struct pollfd local[512];
	nfds_t i;
	int rc;

	if (timeout_ms < 0)
		timeout_ms = 0;
	if (nfds > (qstar_platform_poll_count)(sizeof(local) / sizeof(local[0])))
		return -1;
	for (i = 0; i < (nfds_t)nfds; i++) {
		local[i].fd = fds[i].fd;
		local[i].events = 0;
		if (fds[i].events & QSTAR_PLATFORM_POLLIN)
			local[i].events |= POLLIN;
		local[i].revents = 0;
	}
	for (;;) {
		rc = poll(nfds > 0 ? local : NULL, (nfds_t)nfds, timeout_ms);
		if (rc < 0 && errno == EINTR)
			continue;
		break;
	}
	if (rc > 0) {
		for (i = 0; i < (nfds_t)nfds; i++) {
			fds[i].revents = 0;
			if (local[i].revents & POLLIN)
				fds[i].revents |= QSTAR_PLATFORM_POLLIN;
		}
	}
	return rc;
#endif
}

/** process pipe fd에서 가능한 bytes를 non-blocking 방식으로 읽는다. */
int
qstar_platform_process_read_fd(int fd, char *buf, size_t buflen, size_t *nread,
    int *eof)
{
#if QSTAR_PLATFORM_WINDOWS
#if QSTAR_HAVE_WINDOWS_API
	HANDLE handle;
	DWORD available = 0;
	int n;

	if (nread)
		*nread = 0;
	if (eof)
		*eof = 0;
	if (fd < 0 || !buf || buflen == 0)
		return -1;
	handle = windows_fd_handle(fd);
	if (handle == INVALID_HANDLE_VALUE)
		return -1;
	if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL)) {
		DWORD err = GetLastError();
		if (err == ERROR_BROKEN_PIPE) {
			if (eof)
				*eof = 1;
			return 0;
		}
		return -1;
	}
	if (available == 0)
		return 0;
	if (available > buflen)
		available = (DWORD)buflen;
	n = _read(fd, buf, (unsigned int)available);
	if (n < 0)
		return -1;
	if (nread)
		*nread = (size_t)n;
	if (n == 0 && eof)
		*eof = 1;
	return 0;
#else
	(void)fd;
	(void)buf;
	(void)buflen;
	if (nread)
		*nread = 0;
	if (eof)
		*eof = 1;
	return -1;
#endif
#else
	ssize_t n;

	if (nread)
		*nread = 0;
	if (eof)
		*eof = 0;
	if (fd < 0 || !buf || buflen == 0)
		return -1;
	for (;;) {
		n = read(fd, buf, buflen);
		if (n > 0) {
			if (nread)
				*nread = (size_t)n;
			return 0;
		}
		if (n == 0) {
			if (eof)
				*eof = 1;
			return 0;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return -1;
	}
#endif
}

/** process polling 사이의 짧은 sleep primitive다. */
int
qstar_platform_process_sleep_ms(int timeout_ms)
{
	return qstar_platform_process_poll(NULL, 0, timeout_ms) < 0 ? -1 : 0;
}
