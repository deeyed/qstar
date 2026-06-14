#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
artifact_dir=${QSTAR_DAEMON_ARTIFACT_DIR:-dist/perf}
tmp=${TMPDIR:-/tmp}/qstar-linux-daemon-validation.$$
root=$tmp/project
daemon_dir=${QSTAR_DAEMON_TMPDIR:-$tmp/daemon}
daemon_pid=

status_file="$artifact_dir/linux-daemon-validation-status.txt"
reason_file="$artifact_dir/linux-daemon-validation-reason.txt"
trace_file="$artifact_dir/linux-daemon-validation-trace.txt"
noop_trace_file="$artifact_dir/linux-daemon-validation-noop-trace.txt"
incremental_trace_file="$artifact_dir/linux-daemon-validation-incremental-trace.txt"
server_out_file="$artifact_dir/linux-daemon-validation-server.out"
server_err_file="$artifact_dir/linux-daemon-validation-server.err"

fail() {
	echo "qstar-linux-daemon-validation: $*" >&2
	mkdir -p "$artifact_dir"
	{
		printf 'linux_daemon_validation status=fail reason=%s\n' "$*"
		printf 'host=%s\n' "$(uname -s)"
	} > "$status_file"
	printf '%s\n' "$*" > "$reason_file"
	exit 1
}

contains() {
	file=$1
	pattern=$2
	if ! grep -F -q -- "$pattern" "$file"; then
		sed -n '1,160p' "$file" >&2 || true
		fail "missing pattern '$pattern' in $file"
	fi
}

cleanup() {
	if [ -n "${daemon_pid:-}" ]; then
		kill "$daemon_pid" 2>/dev/null || true
		wait "$daemon_pid" 2>/dev/null || true
	fi
	rm -rf "$tmp"
}

write_project() {
	mkdir -p "$root/src"
	cat > "$root/qstar.lua" <<'EOF'
qstar.project {
  name = "linux-daemon-validation",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.executable "app" {
  sources = {
    "src/main.c",
  },
  description = qstar.status("Linking Linux daemon validation app"),
}
EOF
	cat > "$root/src/main.c" <<'EOF'
int helper(void) { return 41; }
int main(void) { return helper() + 1; }
EOF
}

case "$(uname -s)" in
Linux)
	;;
*)
	mkdir -p "$artifact_dir"
	{
		printf 'linux_daemon_validation status=skipped reason=non-linux host=%s\n' "$(uname -s)"
		printf 'watcher_backend=unavailable\n'
	} > "$status_file"
	printf 'non-linux\n' > "$reason_file"
	printf 'qstar-linux-daemon-validation: skipped host=%s reason=non-linux\n' "$(uname -s)"
	exit 0
	;;
esac

rm -rf "$tmp"
mkdir -p "$artifact_dir" "$root" "$daemon_dir"
chmod 700 "$daemon_dir"
trap cleanup EXIT HUP INT TERM

write_project
daemon_sock="$daemon_dir/qstar-linux-daemon.sock"
rm -f "$daemon_sock"

"$qstar" --file "$root/qstar.lua" -B build/stella-daemon daemon \
	--socket "$daemon_sock" --serve > "$server_out_file" 2> "$server_err_file" &
daemon_pid=$!

i=0
while [ ! -S "$daemon_sock" ] && kill -0 "$daemon_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
	sleep 0.1
	i=$((i + 1))
done

if [ ! -S "$daemon_sock" ]; then
	if grep -E -q "Operation not permitted|Permission denied|permission denied" "$server_err_file"; then
		{
			printf 'linux_daemon_validation status=skipped reason=socket-bind-not-permitted host=Linux\n'
			printf 'watcher_backend=unavailable\n'
		} > "$status_file"
		printf 'socket-bind-not-permitted\n' > "$reason_file"
		printf 'qstar-linux-daemon-validation: skipped reason=socket-bind-not-permitted\n'
		exit 0
	fi
	fail "daemon-socket-not-ready"
fi

"$qstar" --file "$root/qstar.lua" -B build/stella-daemon -G stella \
	build //:app --use-daemon=always --daemon-socket "$daemon_sock" \
	--schedule-trace --progress off --color never > "$trace_file" 2> "$artifact_dir/linux-daemon-validation-trace.err"
contains "$trace_file" "status ok"
contains "$trace_file" "daemon_watcher status=active backend=inotify"
contains "$trace_file" "dirty_state_memory status="
contains "$trace_file" "deps_memory status="

"$qstar" --file "$root/qstar.lua" -B build/stella-daemon -G stella \
	build //:app --use-daemon=always --daemon-socket "$daemon_sock" \
	--schedule-trace --progress off --color never > "$noop_trace_file" 2> "$artifact_dir/linux-daemon-validation-noop-trace.err"
contains "$noop_trace_file" "status ok"
contains "$noop_trace_file" "daemon_watcher status=active backend=inotify"

sleep 1
cat > "$root/src/main.c" <<'EOF'
int helper(void) { return 42; }
int main(void) { return helper() + 2; }
EOF
sleep 1
"$qstar" --file "$root/qstar.lua" -B build/stella-daemon -G stella \
	build //:app --use-daemon=always --daemon-socket "$daemon_sock" \
	--schedule-trace --progress off --color never > "$incremental_trace_file" 2> "$artifact_dir/linux-daemon-validation-incremental-trace.err"
contains "$incremental_trace_file" "status ok"
contains "$incremental_trace_file" "daemon_watcher status=active backend=inotify"
contains "$incremental_trace_file" "daemon_watcher status=event backend=inotify"

{
	printf 'linux_daemon_validation status=ok watcher_backend=inotify host=Linux\n'
	printf 'socket=%s\n' "$daemon_sock"
	grep -m 1 'daemon_watcher status=active backend=inotify' "$trace_file"
	grep -m 1 'daemon_watcher status=event backend=inotify' "$incremental_trace_file"
} > "$status_file"
printf 'ok\n' > "$reason_file"
printf 'qstar-linux-daemon-validation: passed watcher_backend=inotify\n'
