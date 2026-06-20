#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "$qstar" in
/*) ;;
*) qstar=$repo_dir/$qstar ;;
esac

artifact_dir=${QSTAR_DAEMON_BOUNDARY_ARTIFACT_DIR:-dist/perf}
tmp=${QSTAR_DAEMON_BOUNDARY_TMPDIR:-/tmp/qstar-daemon-beta-boundary.$$}
root=$tmp/project
other_root=$tmp/other-project
daemon_dir=$tmp/daemon
log_dir=$artifact_dir/daemon-beta-boundary
status_file=$artifact_dir/daemon-beta-boundary-status.txt
reason_file=$artifact_dir/daemon-beta-boundary-reason.txt
daemon_pid=

fail() {
	echo "qstar-daemon-beta-boundary: $*" >&2
	mkdir -p "$artifact_dir" "$log_dir"
	{
		printf 'daemon_beta_boundary status=fail reason=%s\n' "$*"
		printf 'host=%s\n' "$(uname -s)"
	} > "$status_file"
	printf '%s\n' "$*" > "$reason_file"
	exit 1
}

skip() {
	reason=$1
	mkdir -p "$artifact_dir" "$log_dir"
	{
		printf 'daemon_beta_boundary status=skipped reason=%s host=%s\n' "$reason" "$(uname -s)"
		printf 'socket_policy=unavailable\n'
	} > "$status_file"
	printf '%s\n' "$reason" > "$reason_file"
	printf 'qstar-daemon-beta-boundary: skipped reason=%s\n' "$reason"
	exit 0
}

contains() {
	file=$1
	pattern=$2
	if ! grep -F -q -- "$pattern" "$file"; then
		sed -n '1,180p' "$file" >&2 || true
		fail "missing pattern '$pattern' in $file"
	fi
}

contains_any() {
	base=$1
	pattern=$2
	if grep -F -q -- "$pattern" "$log_dir/$base.out" 2>/dev/null; then
		return 0
	fi
	if grep -F -q -- "$pattern" "$log_dir/$base.err" 2>/dev/null; then
		return 0
	fi
	sed -n '1,160p' "$log_dir/$base.out" >&2 || true
	sed -n '1,160p' "$log_dir/$base.err" >&2 || true
	fail "missing pattern '$pattern' in $base output"
}

run_in() {
	name=$1
	cwd=$2
	shift 2
	(
		cd "$cwd"
		"$@"
	) > "$log_dir/$name.out" 2> "$log_dir/$name.err"
}

run_may_fail_in() {
	name=$1
	cwd=$2
	shift 2
	set +e
	(
		cd "$cwd"
		"$@"
	) > "$log_dir/$name.out" 2> "$log_dir/$name.err"
	rc=$?
	set -e
	return "$rc"
}

find_exe() {
	dir=$1
	name=$2
	found=$(find "$dir" \( -name "$name" -o -name "$name.exe" \) -type f -print -quit 2>/dev/null || true)
	if [ -z "$found" ]; then
		fail "missing executable $name under $dir"
	fi
	printf '%s\n' "$found"
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
  name = "daemon-beta-boundary",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.executable "app" {
  sources = {"src/main.c"},
  description = qstar.status("Linking daemon beta boundary app"),
}
EOF
	cat > "$root/src/main.c" <<'EOF'
#include <stdio.h>

int main(void) {
  puts("daemon-beta-boundary-ok");
  return 0;
}
EOF
}

write_other_project() {
	mkdir -p "$other_root/src"
	cat > "$other_root/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
}
EOF
	cat > "$other_root/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
}

case "$(uname -s)" in
Darwin|Linux)
	;;
*)
	skip "non-unix-socket-host"
	;;
esac

rm -rf "$tmp" "$log_dir"
mkdir -p "$artifact_dir" "$log_dir" "$root" "$daemon_dir"
chmod 700 "$daemon_dir"
trap cleanup EXIT HUP INT TERM
write_project

missing_sock=$daemon_dir/missing.sock
run_in normal-build "$root" "$qstar" --file qstar.lua -B build/normal -G stella \
	build //:app --progress off --color never
contains "$log_dir/normal-build.out" "status ok"

run_in auto-fallback "$root" "$qstar" --file qstar.lua -B build/fallback -G stella \
	build //:app --use-daemon=auto --daemon-socket "$missing_sock" \
	--schedule-trace --progress off --color never
contains "$log_dir/auto-fallback.out" "daemon status=unavailable"
contains "$log_dir/auto-fallback.out" "fallback=stella"
contains "$log_dir/auto-fallback.out" "status ok"

if run_may_fail_in always-missing "$root" "$qstar" --file qstar.lua -B build/always-missing -G stella \
	build //:app --use-daemon=always --daemon-socket "$missing_sock" \
	--progress off --color never; then
	fail "always mode unexpectedly succeeded against missing daemon"
fi
contains_any always-missing "daemon build failed"

bad_dir=$tmp/bad-socket-dir
mkdir -p "$bad_dir"
chmod 0777 "$bad_dir"
if run_may_fail_in bad-dir "$root" "$qstar" --file qstar.lua daemon \
	--socket "$bad_dir/qstar-daemon.sock" --status; then
	fail "insecure daemon socket directory unexpectedly succeeded"
fi
contains_any bad-dir "daemon socket directory must be owner-only"
chmod 700 "$bad_dir"

bad_sock=$daemon_dir/not-a-socket.sock
printf 'not a socket\n' > "$bad_sock"
if run_may_fail_in bad-socket-file "$root" "$qstar" --file qstar.lua daemon \
	--socket "$bad_sock" --serve; then
	fail "non-socket daemon path unexpectedly started"
fi
contains_any bad-socket-file "daemon socket path exists and is not a socket"
contains "$bad_sock" "not a socket"
rm -f "$bad_sock"

daemon_sock=$daemon_dir/qstar-daemon.sock
rm -f "$daemon_sock"
(
	cd "$root"
	"$qstar" --file qstar.lua -B build/daemon daemon --socket "$daemon_sock" --serve
) > "$log_dir/server.out" 2> "$log_dir/server.err" &
daemon_pid=$!

i=0
while [ ! -S "$daemon_sock" ] && kill -0 "$daemon_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
	sleep 0.1
	i=$((i + 1))
done

if [ ! -S "$daemon_sock" ]; then
	if grep -E -q "Operation not permitted|Permission denied|permission denied" "$log_dir/server.err"; then
		skip "socket-bind-not-permitted"
	fi
	fail "daemon-socket-not-ready"
fi

run_in status "$root" "$qstar" --file qstar.lua -B build/daemon daemon \
	--socket "$daemon_sock" --status
contains "$log_dir/status.out" "daemon status=ok experimental=1 pid="
server_real_pid=$(sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' "$log_dir/status.out" | sed -n '1p')

run_in query-hello "$root" "$qstar" --file qstar.lua -B build/daemon daemon \
	--socket "$daemon_sock" --query hello
contains "$log_dir/query-hello.out" "\"schema\":\"qstar-daemon-read-v1\""
contains "$log_dir/query-hello.out" "\"method\":\"hello\""
contains "$log_dir/query-hello.out" "\"readonly\":true"

run_in query-workspace "$root" "$qstar" --file qstar.lua -B build/daemon daemon \
	--socket "$daemon_sock" --query workspace.info
contains "$log_dir/query-workspace.out" "\"method\":\"workspace.info\""
contains "$log_dir/query-workspace.out" "\"build_dir\":\"build/daemon\""

run_in query-targets "$root" "$qstar" --file qstar.lua -B build/daemon daemon \
	--socket "$daemon_sock" --query targets.list
contains "$log_dir/query-targets.out" "\"schema\":\"qstar-targets-v1\""
contains "$log_dir/query-targets.out" "\"label\":\"//:app\""

run_in query-diagnostics "$root" "$qstar" --file qstar.lua -B build/daemon daemon \
	--socket "$daemon_sock" --query diagnostics.list
contains "$log_dir/query-diagnostics.out" "\"method\":\"diagnostics.list\""
contains "$log_dir/query-diagnostics.out" "\"diagnostics\":["

run_in query-compdb "$root" "$qstar" --file qstar.lua -B build/daemon daemon \
	--socket "$daemon_sock" --query compile_commands.path
contains "$log_dir/query-compdb.out" "\"method\":\"compile_commands.path\""

run_in query-summary-missing "$root" "$qstar" --file qstar.lua -B build/daemon daemon \
	--socket "$daemon_sock" --query build.summary
contains "$log_dir/query-summary-missing.out" "\"method\":\"build.summary\""
contains "$log_dir/query-summary-missing.out" "\"exists\":false"

run_in daemon-build "$root" "$qstar" --file qstar.lua -B build/daemon -G stella \
	build //:app --use-daemon=always --daemon-socket "$daemon_sock" \
	--schedule-trace --progress off --color never
contains "$log_dir/daemon-build.out" "status ok"
contains "$log_dir/daemon-build.out" "dirty_state_memory status="
contains "$log_dir/daemon-build.out" "deps_memory status="

normal_exe=$(find_exe "$root/build/normal" app)
daemon_exe=$(find_exe "$root/build/daemon" app)
"$normal_exe" > "$log_dir/normal-app.out" 2> "$log_dir/normal-app.err"
"$daemon_exe" > "$log_dir/daemon-app.out" 2> "$log_dir/daemon-app.err"
contains "$log_dir/normal-app.out" "daemon-beta-boundary-ok"
cmp "$log_dir/normal-app.out" "$log_dir/daemon-app.out" >/dev/null || fail "daemon build output differs from normal Stella build output"

run_in query-summary "$root" "$qstar" --file qstar.lua -B build/daemon daemon \
	--socket "$daemon_sock" --query build.summary
contains "$log_dir/query-summary.out" "\"method\":\"build.summary\""
contains "$log_dir/query-summary.out" "\"exists\":true"
contains "$log_dir/query-summary.out" "\"schema\":\"qstar-build-summary-v1\""

run_in daemon-noop "$root" "$qstar" --file qstar.lua -B build/daemon -G stella \
	build //:app --use-daemon=always --daemon-socket "$daemon_sock" \
	--schedule-trace --progress off --color never
contains "$log_dir/daemon-noop.out" "status ok"
contains "$log_dir/daemon-noop.out" "daemon_server status=build graph=hit"

if run_may_fail_in build-dir-mismatch "$root" "$qstar" --file qstar.lua -B build/other -G stella \
	build //:app --use-daemon=always --daemon-socket "$daemon_sock" \
	--progress off --color never; then
	fail "daemon build_dir mismatch unexpectedly succeeded"
fi
contains_any build-dir-mismatch "daemon identity mismatch: build_dir differs"

write_other_project
if run_may_fail_in root-mismatch "$other_root" "$qstar" --file qstar.lua -B build/daemon -G stella \
	build //:app --use-daemon=always --daemon-socket "$daemon_sock" \
	--progress off --color never; then
	fail "daemon root mismatch unexpectedly succeeded"
fi
contains_any root-mismatch "daemon identity mismatch: package root differs"

if [ -n "$server_real_pid" ]; then
	kill "$server_real_pid" 2>/dev/null || true
	wait "$server_real_pid" 2>/dev/null || true
fi
kill "$daemon_pid" 2>/dev/null || true
wait "$daemon_pid" 2>/dev/null || true
daemon_pid=
sleep 0.2

run_in stale-socket-start "$root" "$qstar" --file qstar.lua -B build/stale-socket daemon \
	--socket "$daemon_sock" --start
contains "$log_dir/stale-socket-start.out" "daemon status=started experimental=1 pid="
run_in stale-socket-stop "$root" "$qstar" --file qstar.lua -B build/stale-socket daemon \
	--socket "$daemon_sock" --stop
contains "$log_dir/stale-socket-stop.out" "daemon status=stopped pid="

stale_pid_dir=$tmp/stale-pid
mkdir -p "$stale_pid_dir"
chmod 700 "$stale_pid_dir"
stale_pid_sock=$stale_pid_dir/qstar-daemon.sock
cat > "$stale_pid_dir/qstar-daemon.pid" <<EOF
qstar-daemon-pid-v1
pid 99999999
socket $stale_pid_sock
EOF
printf 'qstar-daemon-lock-v1\npid 99999999\n' > "$stale_pid_dir/qstar-daemon.lock"
run_in stale-pid-start "$root" "$qstar" --file qstar.lua -B build/stale-pid daemon \
	--socket "$stale_pid_sock" --start
contains "$log_dir/stale-pid-start.out" "daemon cleanup=stale-pid pid=99999999"
contains "$log_dir/stale-pid-start.out" "daemon status=started experimental=1 pid="
run_in stale-pid-stop "$root" "$qstar" --file qstar.lua -B build/stale-pid daemon \
	--socket "$stale_pid_sock" --stop
contains "$log_dir/stale-pid-stop.out" "daemon status=stopped pid="

stale_lock_dir=$tmp/stale-lock
mkdir -p "$stale_lock_dir"
chmod 700 "$stale_lock_dir"
stale_lock_sock=$stale_lock_dir/qstar-daemon.sock
printf 'qstar-daemon-lock-v1\npid 99999999\n' > "$stale_lock_dir/qstar-daemon.lock"
run_in stale-lock-start "$root" "$qstar" --file qstar.lua -B build/stale-lock daemon \
	--socket "$stale_lock_sock" --start
contains "$log_dir/stale-lock-start.out" "daemon cleanup=stale-lock"
contains "$log_dir/stale-lock-start.out" "daemon status=started experimental=1 pid="
run_in stale-lock-stop "$root" "$qstar" --file qstar.lua -B build/stale-lock daemon \
	--socket "$stale_lock_sock" --stop
contains "$log_dir/stale-lock-stop.out" "daemon status=stopped pid="

{
	printf 'daemon_beta_boundary status=ok host=%s\n' "$(uname -s)"
	printf 'socket=%s\n' "$daemon_sock"
	grep -m 1 'daemon status=ok experimental=1 pid=' "$log_dir/status.out"
	grep -m 1 'daemon status=unavailable' "$log_dir/auto-fallback.out"
	grep -m 1 'dirty_state_memory status=' "$log_dir/daemon-build.out"
	grep -m 1 'deps_memory status=' "$log_dir/daemon-build.out"
	printf 'read_api=hello,workspace.info,targets.list,diagnostics.list,compile_commands.path,build.summary\n'
	printf 'normal_stella_parity=ok\n'
	printf 'socket_permission_regression=ok\n'
	printf 'identity_mismatch_regression=ok\n'
	printf 'stale_lifecycle_regression=ok\n'
} > "$status_file"
printf 'ok\n' > "$reason_file"
printf 'qstar-daemon-beta-boundary: passed\n'
