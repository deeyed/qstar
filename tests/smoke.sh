#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-smoke.$$
group_tmp=${TMPDIR:-/tmp}/qstar-smoke-group.$$
last_step="setup"
last_prefix=
dumped=0
daemon_pid=

dump_file_tail() {
	file=$1
	lines=${QSTAR_SMOKE_TAIL_LINES:-80}
	if [ ! -f "$file" ]; then
		return 0
	fi
	echo "qstar-smoke: --- $file (tail -n $lines) ---" >&2
	tail -n "$lines" "$file" >&2 || true
}

dump_related_file() {
	file=$1
	dump_file_tail "$file"
	case "$file" in
		*.out)
			dump_file_tail "${file%.out}.err"
			;;
		*.err)
			dump_file_tail "${file%.err}.out"
			;;
		*.combined)
			;;
	esac
}

dump_recent_outputs() {
	max=${QSTAR_SMOKE_DUMP_FILES:-12}
	files=$(find "$tmp" "$group_tmp" -type f \( -name '*.out' -o -name '*.err' -o -name '*.combined' \) -print 2>/dev/null | sort | tail -n "$max" || true)
	if [ -z "$files" ]; then
		return 0
	fi
	echo "qstar-smoke: recent captured outputs:" >&2
	printf '%s\n' "$files" | while IFS= read -r file; do
		dump_file_tail "$file"
	done
}

dump_step_files() {
	if [ "$dumped" -ne 0 ]; then
		return 0
	fi
	dumped=1
	if [ -n "$last_prefix" ]; then
		dump_related_file "$tmp/$last_prefix.out"
		dump_related_file "$group_tmp/$last_prefix.out"
		dump_file_tail "$tmp/$last_prefix.combined"
		dump_file_tail "$group_tmp/$last_prefix.combined"
	else
		dump_recent_outputs
	fi
}

fail() {
	echo "qstar-smoke: $*" >&2
	dump_step_files
	exit 1
}

step() {
	last_step=$1
	last_prefix=${2:-}
}

cleanup() {
	rc=$?
	if [ -n "${daemon_pid:-}" ]; then
		kill "$daemon_pid" 2>/dev/null || true
		wait "$daemon_pid" 2>/dev/null || true
	fi
	if [ "$rc" -ne 0 ]; then
		echo "qstar-smoke: failed during $last_step (exit $rc)" >&2
		dump_step_files
	fi
	rm -rf "$tmp" "$group_tmp"
	exit "$rc"
}

contains() {
	file=$1
	pat=$2
	if ! grep -F -q -- "$pat" "$file"; then
		echo "qstar-smoke: missing pattern '$pat' in $file" >&2
		dump_related_file "$file"
		dumped=1
		fail "missing pattern '$pat' in $file"
	fi
}

not_contains() {
	file=$1
	pat=$2
	if grep -F -q -- "$pat" "$file"; then
		echo "qstar-smoke: unexpected pattern '$pat' in $file" >&2
		dump_related_file "$file"
		dumped=1
		fail "unexpected pattern '$pat' in $file"
	fi
}

send_lsp() {
	payload=$1
	len=$(printf "%s" "$payload" | wc -c | tr -d ' ')
	printf 'Content-Length: %s\r\n\r\n%s' "$len" "$payload"
}

rm -rf "$tmp" "$group_tmp"
mkdir -p "$tmp/src" "$tmp/tools"
trap cleanup EXIT HUP INT TERM

step "initial build smoke setup"
cat > "$tmp/qstar.lua" <<'EOF'
qstar.project {
  name = "smoke",
  version = "0.1.0",
  root = ".",
}

qstar.executable "app" {
  sources = {"src/main.c"},
}

qstar.run_target "smoke" {
  deps = {"//:app"},
  command = qstar.cli {qstar.target_file("//:app")},
  description = qstar.status("Running smoke binary"),
}

qstar.run_target "fail" {
  command = qstar.cli {"tools/fail.sh"},
  description = qstar.status("Running failing smoke"),
}
EOF

cat > "$tmp/src/main.c" <<'EOF'
#include "dep.h"
int main(void) { return SMOKE_VALUE; }
EOF
cat > "$tmp/src/dep.h" <<'EOF'
#define SMOKE_VALUE 0
EOF
cat > "$tmp/tools/fail.sh" <<'EOF'
#!/bin/sh
exit 7
EOF
chmod +x "$tmp/tools/fail.sh"

step "initial build smoke: schedule trace build" "first"
"$qstar" --file "$tmp/qstar.lua" build //:app --jobs 2 --schedule-trace > "$tmp/first.out" 2> "$tmp/first.err"
contains "$tmp/first.out" "plan_cache status=miss reason=manifest-missing"
contains "$tmp/first.out" "qstar build v2"
contains "$tmp/first.out" "executor-policy version=v4"
contains "$tmp/first.out" "parallel=optional jobs=2"
contains "$tmp/first.out" "action_dag target=//:app"
contains "$tmp/first.out" "action_description id=//:app:compile:0 text=\"Building C object build/qstar/out/___app/obj0.o\""
contains "$tmp/first.out" "schedule_action id=//:app:compile:0"
case "$(uname -s)" in
  Darwin|Linux) contains "$tmp/first.out" "runner=posix_spawn" ;;
  *) contains "$tmp/first.out" "runner=" ;;
esac
contains "$tmp/first.out" "status=run"
contains "$tmp/first.out" "[ 50%] Building C object build/qstar/out/___app/obj0.o"
contains "$tmp/first.out" "[100%] Linking C executable build/qstar/out/___app/app"
contains "$tmp/first.out" "[100%] Built target app"
not_contains "$tmp/first.out" "Stage 1: prepare"
contains "$tmp/first.out" "status ok"

host_jobs=1
case "$(uname -s)" in
  Darwin|Linux)
    host_jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
    ;;
esac
case "$host_jobs" in
  ''|*[!0-9]*) host_jobs=1 ;;
esac
if [ "$host_jobs" -gt 1 ]; then
  step "initial build smoke: default jobs use host cpu count" "default-jobs"
  "$qstar" --file "$tmp/qstar.lua" -B build/default-jobs build //:app --schedule-trace > "$tmp/default-jobs.out" 2> "$tmp/default-jobs.err"
  contains "$tmp/default-jobs.out" "executor-policy version=v4"
  contains "$tmp/default-jobs.out" "parallel=optional jobs="
  contains "$tmp/default-jobs.out" "active=action-dag-ready-queue"
  not_contains "$tmp/default-jobs.out" "parallel=no jobs=1 active=serial-ready-queue"
fi

test -f "$tmp/build/qstar/state/state.db" || fail "missing compact action state"
test -f "$tmp/build/qstar/state/deps.db" || fail "missing compact dependency state"
test -f "$tmp/build/qstar/state/graph.json" || fail "schedule trace graph snapshot missing"
test -f "$tmp/build/qstar/state/last-summary.json" || fail "schedule trace build summary missing"
test ! -e "$tmp/build/qstar/state/actions.json" || fail "debug action state dump should be opt-in"
"$qstar" --file "$tmp/qstar.lua" -B build/metadata-default build //:app --progress off > "$tmp/metadata-default.out" 2> "$tmp/metadata-default.err"
test -f "$tmp/build/metadata-default/state/state.db" || fail "metadata default compact action state missing"
test -f "$tmp/build/metadata-default/state/deps.db" || fail "metadata default compact deps missing"
test ! -e "$tmp/build/metadata-default/state/graph.json" || fail "default graph snapshot should be debug opt-in"
test ! -e "$tmp/build/metadata-default/state/last-summary.json" || fail "default success summary should be debug opt-in"
test ! -e "$tmp/build/metadata-default/state/actions.json" || fail "default debug action state should be opt-in"
QSTAR_DEBUG_STATE_DUMPS=1 "$qstar" --file "$tmp/qstar.lua" -B build/metadata-debug build //:app --progress off > "$tmp/debug-state.out" 2> "$tmp/debug-state.err"
test -f "$tmp/build/metadata-debug/state/graph.json" || fail "missing opt-in graph snapshot"
test -f "$tmp/build/metadata-debug/state/last-summary.json" || fail "missing opt-in build summary"
test -f "$tmp/build/metadata-debug/state/actions.json" || fail "missing opt-in debug action state"
contains "$tmp/build/metadata-debug/state/graph.json" "\"schema\":\"qstar-graph-snapshot-v1\""
contains "$tmp/build/metadata-debug/state/graph.json" "\"project\":{\"name\":\"smoke\""
contains "$tmp/build/metadata-debug/state/graph.json" "\"label\":\"//:app\""
contains "$tmp/build/metadata-debug/state/last-summary.json" "\"schema\":\"qstar-build-summary-v1\""
contains "$tmp/build/metadata-debug/state/last-summary.json" "\"status\":\"success\""
contains "$tmp/build/metadata-debug/state/actions.json" "\"argv_key\":"
contains "$tmp/build/metadata-debug/state/actions.json" "\"env_key\":"
contains "$tmp/build/metadata-debug/state/actions.json" "\"input_key\":"
contains "$tmp/build/metadata-debug/state/actions.json" "\"output_key\":"
contains "$tmp/build/metadata-debug/state/actions.json" "\"external_tool_key\":"
test -f "$tmp/build/qstar/stella/manifest.json" || fail "missing Stella plan cache manifest"
test -f "$tmp/build/qstar/stella/inputs.json" || fail "missing Stella plan cache inputs"
test -f "$tmp/build/qstar/stella/graph.qsg" || fail "missing Stella graph cache"
test -f "$tmp/build/qstar/stella/actions.qsa" || fail "missing Stella action plan cache"
contains "$tmp/build/qstar/stella/manifest.json" "\"schema\":\"qstar-stella-plan-cache-v1\""
contains "$tmp/build/qstar/stella/actions.qsa" "qstar-stella-actions-cache-v1"
test -f "$tmp/build/qstar/compile_commands.json" || fail "missing compile_commands.json"
contains "$tmp/build/qstar/compile_commands.json" "src/main.c"
test ! -f "$tmp/compile_commands.json" || fail "default compile_commands leaked to root"

step "initial build smoke: Stella plan cache hit" "plan-cache-hit"
"$qstar" --file "$tmp/qstar.lua" build //:app --progress off --schedule-trace > "$tmp/plan-cache-hit.out" 2> "$tmp/plan-cache-hit.err"
contains "$tmp/plan-cache-hit.out" "plan_cache status=hit reason=hit"
contains "$tmp/plan-cache-hit.out" "lowered_action id=//:app:compile:0 status=hit kind=compile"
contains "$tmp/plan-cache-hit.out" "lowered_action id=//:app:link:0 status=hit kind=link"
contains "$tmp/plan-cache-hit.out" "dirty_state_db status=hit"
contains "$tmp/plan-cache-hit.out" "deps_db status=hit"
contains "$tmp/plan-cache-hit.out" "build_action id=//:app:compile:0 status=skip reason=cache-hit"
contains "$tmp/plan-cache-hit.out" "build_action id=//:app:link:0 status=skip reason=cache-hit"
contains "$tmp/plan-cache-hit.out" "status ok run=0 skip=2 fail=0"

step "initial build smoke: Stella plan cache invalidation" "plan-cache-miss"
printf '%s\n' '-- plan cache invalidation smoke' >> "$tmp/qstar.lua"
"$qstar" --file "$tmp/qstar.lua" build //:app --progress off --schedule-trace > "$tmp/plan-cache-miss.out" 2> "$tmp/plan-cache-miss.err"
contains "$tmp/plan-cache-miss.out" "plan_cache status=miss reason=authoring-input-changed"
contains "$tmp/plan-cache-miss.out" "status ok"

step "initial build smoke: daemon socket security" "daemon-security"
daemon_dir="$tmp/daemon-secure"
mkdir -p "$daemon_dir"
chmod 700 "$daemon_dir"
if "$qstar" --file "$tmp/qstar.lua" daemon --socket "tcp://127.0.0.1:9417" --status > "$tmp/daemon-remote.out" 2> "$tmp/daemon-remote.err"; then
	fail "remote daemon socket path unexpectedly succeeded"
fi
contains "$tmp/daemon-remote.out" "daemon status=unavailable"
contains "$tmp/daemon-remote.out" "remote daemon socket paths are not supported"
daemon_insecure_dir="$tmp/daemon-insecure"
mkdir -p "$daemon_insecure_dir"
chmod 755 "$daemon_insecure_dir"
if "$qstar" --file "$tmp/qstar.lua" daemon --socket "$daemon_insecure_dir/qstar-daemon.sock" --status > "$tmp/daemon-insecure.out" 2> "$tmp/daemon-insecure.err"; then
	fail "insecure daemon socket directory unexpectedly succeeded"
fi
contains "$tmp/daemon-insecure.out" "daemon status=unavailable"
contains "$tmp/daemon-insecure.out" "daemon socket directory must be owner-only"
daemon_bad_sock="$daemon_dir/not-a-socket.sock"
printf 'not a socket\n' > "$daemon_bad_sock"
if "$qstar" --file "$tmp/qstar.lua" daemon --socket "$daemon_bad_sock" --serve > "$tmp/daemon-stale-file.out" 2> "$tmp/daemon-stale-file.err"; then
	fail "non-socket daemon path unexpectedly started a server"
fi
contains "$tmp/daemon-stale-file.err" "daemon socket path exists and is not a socket"
contains "$daemon_bad_sock" "not a socket"
rm -f "$daemon_bad_sock"

step "initial build smoke: experimental Stella daemon" "daemon"
daemon_sock="$daemon_dir/qstar-daemon.sock"
rm -f "$daemon_sock"
"$qstar" --file "$tmp/qstar.lua" daemon --socket "$daemon_sock" --serve > "$tmp/daemon-server.out" 2> "$tmp/daemon-server.err" &
daemon_pid=$!
i=0
while [ ! -S "$daemon_sock" ] && kill -0 "$daemon_pid" 2>/dev/null && [ "$i" -lt 30 ]; do
	sleep 0.1
	i=$((i + 1))
done
if [ -S "$daemon_sock" ]; then
	"$qstar" --file "$tmp/qstar.lua" daemon --socket "$daemon_sock" --status > "$tmp/daemon-status.out" 2> "$tmp/daemon-status.err"
	contains "$tmp/daemon-status.out" "daemon status=ok experimental=1"
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon daemon --socket "$daemon_sock" --query hello > "$tmp/daemon-query-hello.out" 2> "$tmp/daemon-query-hello.err"
	contains "$tmp/daemon-query-hello.out" "\"schema\":\"qstar-daemon-read-v1\""
	contains "$tmp/daemon-query-hello.out" "\"method\":\"hello\""
	contains "$tmp/daemon-query-hello.out" "\"readonly\":true"
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon daemon --socket "$daemon_sock" --query workspace.info > "$tmp/daemon-query-workspace.out" 2> "$tmp/daemon-query-workspace.err"
	contains "$tmp/daemon-query-workspace.out" "\"method\":\"workspace.info\""
	contains "$tmp/daemon-query-workspace.out" "\"build_dir\":\"build/daemon\""
	contains "$tmp/daemon-query-workspace.out" "\"target_count\":3"
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon daemon --socket "$daemon_sock" --query targets.list > "$tmp/daemon-query-targets.out" 2> "$tmp/daemon-query-targets.err"
	contains "$tmp/daemon-query-targets.out" "\"schema\":\"qstar-targets-v1\""
	contains "$tmp/daemon-query-targets.out" "\"label\":\"//:app\""
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon daemon --socket "$daemon_sock" --query diagnostics.list > "$tmp/daemon-query-diagnostics.out" 2> "$tmp/daemon-query-diagnostics.err"
	contains "$tmp/daemon-query-diagnostics.out" "\"method\":\"diagnostics.list\""
	contains "$tmp/daemon-query-diagnostics.out" "\"diagnostics\":["
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon daemon --socket "$daemon_sock" --query compile_commands.path > "$tmp/daemon-query-compdb.out" 2> "$tmp/daemon-query-compdb.err"
	contains "$tmp/daemon-query-compdb.out" "\"method\":\"compile_commands.path\""
	contains "$tmp/daemon-query-compdb.out" "\"path\":\"build/daemon/compile_commands.json\""
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon daemon --socket "$daemon_sock" --query build.summary > "$tmp/daemon-query-summary-missing.out" 2> "$tmp/daemon-query-summary-missing.err"
	contains "$tmp/daemon-query-summary-missing.out" "\"method\":\"build.summary\""
	contains "$tmp/daemon-query-summary-missing.out" "\"exists\":false"
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon build //:app --use-daemon=always --daemon-socket "$daemon_sock" --schedule-trace --progress off --color never > "$tmp/daemon-first.out" 2> "$tmp/daemon-first.err"
	contains "$tmp/daemon-first.out" "daemon_server status=build graph=miss"
	contains "$tmp/daemon-first.out" "daemon_watcher status=active"
	contains "$tmp/daemon-first.out" "dirty_state_memory status=miss reason=cold"
	contains "$tmp/daemon-first.out" "dirty_state_memory status=writeback"
	contains "$tmp/daemon-first.out" "status ok"
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon daemon --socket "$daemon_sock" --query build.summary > "$tmp/daemon-query-summary.out" 2> "$tmp/daemon-query-summary.err"
	contains "$tmp/daemon-query-summary.out" "\"method\":\"build.summary\""
	contains "$tmp/daemon-query-summary.out" "\"exists\":true"
	contains "$tmp/daemon-query-summary.out" "\"schema\":\"qstar-build-summary-v1\""
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon build //:app --use-daemon=always --daemon-socket "$daemon_sock" --schedule-trace --progress off --color never > "$tmp/daemon-second.out" 2> "$tmp/daemon-second.err"
	contains "$tmp/daemon-second.out" "daemon_server status=build graph=hit reason=memory experimental=1"
	contains "$tmp/daemon-second.out" "daemon_watcher status=active"
	contains "$tmp/daemon-second.out" "dirty_state_memory status=hit"
	contains "$tmp/daemon-second.out" "deps_memory status=hit"
	contains "$tmp/daemon-second.out" "status ok"
	printf '%s\n' '-- daemon watcher authoring invalidation smoke' >> "$tmp/qstar.lua"
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon build //:app --use-daemon=always --daemon-socket "$daemon_sock" --schedule-trace --progress off --color never > "$tmp/daemon-authoring.out" 2> "$tmp/daemon-authoring.err"
	contains "$tmp/daemon-authoring.out" "daemon_watcher status=event"
	contains "$tmp/daemon-authoring.out" "scope=authoring"
	contains "$tmp/daemon-authoring.out" "daemon_server status=build graph=miss reason=watch-authoring-changed experimental=1"
	contains "$tmp/daemon-authoring.out" "status ok"
	printf '#define SMOKE_VALUE 1\n' > "$tmp/src/dep.h"
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon build //:app --use-daemon=always --daemon-socket "$daemon_sock" --schedule-trace --progress off --color never > "$tmp/daemon-incremental.out" 2> "$tmp/daemon-incremental.err"
	contains "$tmp/daemon-incremental.out" "daemon_watcher status=event"
	contains "$tmp/daemon-incremental.out" "scope=input"
	contains "$tmp/daemon-incremental.out" "daemon_server status=build graph=hit reason=memory experimental=1"
	contains "$tmp/daemon-incremental.out" "status ok"
	printf '#define SMOKE_VALUE 0\n' > "$tmp/src/dep.h"
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon build //:app --use-daemon=always --daemon-socket "$daemon_sock" --progress plain --color never > "$tmp/daemon-ui.out" 2> "$tmp/daemon-ui.err"
	contains "$tmp/daemon-ui.out" "[100%] Built target app"
	contains "$tmp/daemon-ui.out" "status ok"
	not_contains "$tmp/daemon-ui.out" "qstar-daemon-stream"
	not_contains "$tmp/daemon-ui.out" "event progress"
	if "$qstar" --file "$tmp/qstar.lua" -B build/daemon-ui build //:app --use-daemon=always --daemon-socket "$daemon_sock" --progress off --color never > "$tmp/daemon-mismatch.out" 2> "$tmp/daemon-mismatch.err"; then
		fail "daemon build_dir mismatch unexpectedly succeeded"
	fi
	contains "$tmp/daemon-mismatch.out" "daemon identity mismatch: build_dir differs"
	kill "$daemon_pid" 2>/dev/null || true
	wait "$daemon_pid" 2>/dev/null || true
	daemon_pid=
	rm -f "$daemon_sock"
	lifecycle_sock="$daemon_dir/qd.sock"
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon-lifecycle daemon --socket "$lifecycle_sock" --start > "$tmp/daemon-lifecycle-start.out" 2> "$tmp/daemon-lifecycle-start.err"
	contains "$tmp/daemon-lifecycle-start.out" "daemon status=started experimental=1 pid="
	contains "$tmp/daemon-lifecycle-start.out" "socket=$lifecycle_sock"
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon-lifecycle daemon --socket "$lifecycle_sock" --status > "$tmp/daemon-lifecycle-status.out" 2> "$tmp/daemon-lifecycle-status.err"
	contains "$tmp/daemon-lifecycle-status.out" "daemon status=ok experimental=1 pid="
	if "$qstar" --file "$tmp/qstar.lua" -B build/daemon-lifecycle daemon --socket "$lifecycle_sock" --start > "$tmp/daemon-lifecycle-start2.out" 2> "$tmp/daemon-lifecycle-start2.err"; then
		fail "daemon duplicate start unexpectedly succeeded"
	fi
	contains "$tmp/daemon-lifecycle-start2.err" "daemon already running"
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon-lifecycle daemon --socket "$lifecycle_sock" --query hello > "$tmp/daemon-lifecycle-query.out" 2> "$tmp/daemon-lifecycle-query.err"
	contains "$tmp/daemon-lifecycle-query.out" "\"method\":\"hello\""
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon-lifecycle daemon --socket "$lifecycle_sock" --stop > "$tmp/daemon-lifecycle-stop.out" 2> "$tmp/daemon-lifecycle-stop.err"
	contains "$tmp/daemon-lifecycle-stop.out" "daemon status=stopped pid="
	"$qstar" --file "$tmp/qstar.lua" -B build/daemon-lifecycle daemon --socket "$lifecycle_sock" --status > "$tmp/daemon-lifecycle-stopped.out" 2> "$tmp/daemon-lifecycle-stopped.err" || true
	contains "$tmp/daemon-lifecycle-stopped.out" "daemon status=unavailable reason=socket-missing"
else
	if grep -F -q "Operation not permitted" "$tmp/daemon-server.err"; then
		echo "qstar-smoke: experimental daemon socket smoke skipped: sandbox disallows Unix socket bind" >&2
		kill "$daemon_pid" 2>/dev/null || true
		wait "$daemon_pid" 2>/dev/null || true
		daemon_pid=
	else
		fail "experimental daemon socket did not become ready"
	fi
fi
"$qstar" --file "$tmp/qstar.lua" -B build/daemon-fallback build //:app --use-daemon=auto --daemon-socket "$daemon_sock" --schedule-trace --progress off --color never > "$tmp/daemon-fallback.out" 2> "$tmp/daemon-fallback.err"
contains "$tmp/daemon-fallback.out" "daemon status=unavailable"
contains "$tmp/daemon-fallback.out" "fallback=stella"
contains "$tmp/daemon-fallback.out" "status ok"

step "initial build smoke: action descriptions" "action-description-dry"
"$qstar" --file "$tmp/qstar.lua" dry-run //:smoke > "$tmp/action-description-dry.out" 2> "$tmp/action-description-dry.err"
contains "$tmp/action-description-dry.out" "action_description id=//:app:compile:0 text=\"Building C object build/qstar/out/___app/obj0.o\""
contains "$tmp/action-description-dry.out" "action_description id=//:app:link:0 text=\"Linking C executable build/qstar/out/___app/app\""
contains "$tmp/action-description-dry.out" "action_description id=//:smoke:run:0 text=\"Running smoke binary\""
step "initial build smoke: explain descriptions" "action-description-explain"
"$qstar" --file "$tmp/qstar.lua" explain //:app > "$tmp/action-description-explain.out" 2> "$tmp/action-description-explain.err"
contains "$tmp/action-description-explain.out" "action_description id=//:app:compile:0 text=\"Building C object build/qstar/out/___app/obj0.o\""
contains "$tmp/action-description-explain.out" "action_description id=//:app:link:0 text=\"Linking C executable build/qstar/out/___app/app\""
step "initial build smoke: verbose descriptions" "action-description-build"
"$qstar" --file "$tmp/qstar.lua" build //:app --verbose --progress plain --color never > "$tmp/action-description-build.out" 2> "$tmp/action-description-build.err"
contains "$tmp/action-description-build.out" "action_description id=//:app:compile:0 text=\"Building C object build/qstar/out/___app/obj0.o\""
contains "$tmp/action-description-build.out" "action_description id=//:app:link:0 text=\"Linking C executable build/qstar/out/___app/app\""

step "initial build smoke: compact progress" "ui-compact"
"$qstar" --file "$tmp/qstar.lua" build //:app --progress plain --color never > "$tmp/ui-compact.out" 2> "$tmp/ui-compact.err"
contains "$tmp/ui-compact.out" "[100%] Built target app"
contains "$tmp/ui-compact.out" "status ok"
not_contains "$tmp/ui-compact.out" "build_action "
not_contains "$tmp/ui-compact.out" "action_description "
not_contains "$tmp/ui-compact.out" "Status: "
not_contains "$tmp/ui-compact.out" "Stage "
step "initial build smoke: verbose progress" "ui-verbose"
"$qstar" --file "$tmp/qstar.lua" build //:app --verbose --progress plain --color never > "$tmp/ui-verbose.out" 2> "$tmp/ui-verbose.err"
contains "$tmp/ui-verbose.out" "Status: compiling //:app"
contains "$tmp/ui-verbose.out" "build_action id=//:app:compile:0"
contains "$tmp/ui-verbose.out" "[100%] Built target app"
not_contains "$tmp/ui-verbose.out" "Stage "
esc=$(printf '\033')
step "initial build smoke: color and progress off" "ui-color"
"$qstar" --file "$tmp/qstar.lua" build //:app --progress off --color always > "$tmp/ui-color.out" 2> "$tmp/ui-color.err"
contains "$tmp/ui-color.out" "${esc}[32mstatus ok${esc}[0m"
not_contains "$tmp/ui-color.out" "[100%]"
not_contains "$tmp/ui-color.out" "Building C object"

step "initial build smoke: run target log/replay" "log-run"
"$qstar" --file "$tmp/qstar.lua" build //:smoke --progress plain --color never > "$tmp/log-run.out" 2> "$tmp/log-run.err"
contains "$tmp/log-run.out" "Running smoke binary"
step "initial build smoke: compile action log" "action-log-compile"
"$qstar" --file "$tmp/qstar.lua" action-log //:app:compile:0 > "$tmp/action-log-compile.out" 2> "$tmp/action-log-compile.err"
contains "$tmp/action-log-compile.out" "description='Building C object build/qstar/out/___app/obj0.o'"
test ! -f "$tmp/build/qstar/logs/___app_compile_0.log" || fail "successful compile log was eagerly materialized"
step "initial build smoke: compile replay" "replay-compile"
"$qstar" --file "$tmp/qstar.lua" replay //:app:compile:0 > "$tmp/replay-compile.out" 2> "$tmp/replay-compile.err"
contains "$tmp/replay-compile.out" "description='Building C object build/qstar/out/___app/obj0.o'"
step "initial build smoke: run action log" "action-log-run"
"$qstar" --file "$tmp/qstar.lua" action-log //:smoke:run:0 > "$tmp/action-log-run.out" 2> "$tmp/action-log-run.err"
contains "$tmp/action-log-run.out" "description='Running smoke binary'"
step "initial build smoke: run replay" "replay-run"
"$qstar" --file "$tmp/qstar.lua" replay //:smoke:run:0 > "$tmp/replay-run.out" 2> "$tmp/replay-run.err"
contains "$tmp/replay-run.out" "description='Running smoke binary'"
step "initial build smoke: failing run target" "fail-run"
if "$qstar" --file "$tmp/qstar.lua" build //:fail --progress off --color never > "$tmp/fail-run.out" 2> "$tmp/fail-run.err"; then
	fail "failing run_target unexpectedly succeeded"
fi
contains "$tmp/fail-run.out" "run_target_result label=//:fail status=exit-code"
step "initial build smoke: last failure" "last-failure"
"$qstar" --file "$tmp/qstar.lua" last-failure > "$tmp/last-failure.out" 2> "$tmp/last-failure.err"
contains "$tmp/last-failure.out" "description='Running failing smoke'"
step "initial build smoke: failing run replay" "replay-fail"
"$qstar" --file "$tmp/qstar.lua" replay //:fail:run:0 > "$tmp/replay-fail.out" 2> "$tmp/replay-fail.err"
contains "$tmp/replay-fail.out" "description='Running failing smoke'"

step "diagnostic stream warnings" "diagnostic-stream"
mkdir -p "$tmp/diagnostic-stream/src" "$tmp/diagnostic-stream/tools"
cat > "$tmp/diagnostic-stream/qstar.lua" <<'EOF'
qstar.project {
  name = "diagnostic-stream",
  version = "0.1.0",
  root = ".",
}

qstar.profile "default" {
  cc = "tools/warn-cc.sh",
}

qstar.staticlib "core" {
  sources = {"src/core.c"},
}
EOF
cat > "$tmp/diagnostic-stream/src/core.c" <<'EOF'
int core(void) { return 0; }
EOF
cat > "$tmp/diagnostic-stream/tools/warn-cc.sh" <<'EOF'
#!/bin/sh
set -eu
out=
dep=
src=
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o)
      shift
      out=$1
      ;;
    -MF)
      shift
      dep=$1
      ;;
    -c)
      shift
      src=$1
      ;;
  esac
  shift || break
done
printf 'warning: stdout diagnostic for %s\n' "$src"
printf 'warning: stderr diagnostic for %s\n' "$src" >&2
mkdir -p "$(dirname "$out")"
printf 'fake object\n' > "$out"
if [ -n "$dep" ]; then
  mkdir -p "$(dirname "$dep")"
  printf "%s: %s\n" "$out" "$src" > "$dep"
fi
EOF
chmod +x "$tmp/diagnostic-stream/tools/warn-cc.sh"
"$qstar" --file "$tmp/diagnostic-stream/qstar.lua" build //:core --progress plain --color never > "$tmp/diagnostic-stream.out" 2> "$tmp/diagnostic-stream.err"
contains "$tmp/diagnostic-stream.out" "[ 50%] Building C object build/qstar/out/___core/obj0.o"
contains "$tmp/diagnostic-stream.out" "warning: stdout diagnostic for src/core.c"
contains "$tmp/diagnostic-stream.out" "warning: stderr diagnostic for src/core.c"
contains "$tmp/diagnostic-stream.out" "status ok"
not_contains "$tmp/diagnostic-stream.out" "$esc"
contains "$tmp/diagnostic-stream/build/qstar/logs/___core_compile_0.stdout" "warning: stdout diagnostic for src/core.c"
contains "$tmp/diagnostic-stream/build/qstar/logs/___core_compile_0.stderr" "warning: stderr diagnostic for src/core.c"
not_contains "$tmp/diagnostic-stream/build/qstar/logs/___core_compile_0.stderr" "$esc"
"$qstar" --file "$tmp/diagnostic-stream/qstar.lua" -B build-color build //:core --progress plain --color always > "$tmp/diagnostic-stream-color.out" 2> "$tmp/diagnostic-stream-color.err"
contains "$tmp/diagnostic-stream-color.out" "${esc}[1;33mwarning:${esc}[0m stdout diagnostic for src/core.c"
contains "$tmp/diagnostic-stream-color.out" "${esc}[1;33mwarning:${esc}[0m stderr diagnostic for src/core.c"

step "diagnostic stream errors" "error-stream"
mkdir -p "$tmp/error-stream/src" "$tmp/error-stream/tools"
cat > "$tmp/error-stream/qstar.lua" <<'EOF'
qstar.project {
  name = "error-stream",
  version = "0.1.0",
  root = ".",
}

qstar.profile "default" {
  cc = "tools/error-cc.sh",
}

qstar.staticlib "core" {
  sources = {"src/core.c"},
}
EOF
cat > "$tmp/error-stream/src/core.c" <<'EOF'
int core(void) { return 0; }
EOF
cat > "$tmp/error-stream/tools/error-cc.sh" <<'EOF'
#!/bin/sh
printf 'error: forced compile failure\n' >&2
exit 7
EOF
chmod +x "$tmp/error-stream/tools/error-cc.sh"
if "$qstar" --file "$tmp/error-stream/qstar.lua" build //:core --progress plain --color always > "$tmp/error-stream.out" 2> "$tmp/error-stream.err"; then
	fail "error stream compiler unexpectedly succeeded"
fi
contains "$tmp/error-stream.out" "${esc}[1;31merror:${esc}[0m forced compile failure"
contains "$tmp/error-stream.out" "action_diagnostic_json"
if grep '^action_diagnostic_json ' "$tmp/error-stream.out" | grep -q "$esc"; then
	fail "action diagnostic json contains ANSI color"
fi
contains "$tmp/error-stream/build/qstar/logs/___core_compile_0.stderr" "error: forced compile failure"
not_contains "$tmp/error-stream/build/qstar/logs/___core_compile_0.stderr" "$esc"

step "group target corpus" "group-build"
mkdir -p "$group_tmp/group/lib/libk" "$group_tmp/group/sys/kern" "$group_tmp/group/sys/kern/mm" "$group_tmp/group/sys/kern/irq"
cat > "$group_tmp/group/qstar.lua" <<'EOF'
qstar.project {
  name = "group-corpus",
  version = "0.1.0",
  root = ".",
}

qstar.subdir("lib/libk")
qstar.subdir("sys/kern/mm")
qstar.subdir("sys/kern/irq")
qstar.subdir("sys/kern")

qstar.group "firmware_image" {
  deps = {
    "//sys/kern:kernel_subsystems",
  },
}
EOF
cat > "$group_tmp/group/lib/libk/libk.qst" <<'EOF'
qstar.staticlib "libk_core" {
  sources = {
    "lib/libk/libk.c",
  },
}
EOF
cat > "$group_tmp/group/sys/kern/mm/mm.qst" <<'EOF'
qstar.staticlib "kernel_mm" {
  sources = {
    "sys/kern/mm/mm.c",
  },
  deps = {
    "//lib/libk:libk_core",
  },
}
EOF
cat > "$group_tmp/group/sys/kern/irq/irq.qst" <<'EOF'
qstar.staticlib "kernel_irq" {
  sources = {
    "sys/kern/irq/irq.c",
  },
  deps = {
    "//lib/libk:libk_core",
  },
}
EOF
cat > "$group_tmp/group/sys/kern/kern.qst" <<'EOF'
qstar.group "kernel_subsystems" {
  deps = {
    "//sys/kern/mm:kernel_mm",
    "//sys/kern/irq:kernel_irq",
  },
}
EOF
cat > "$group_tmp/group/lib/libk/libk.c" <<'EOF'
int libk_core(void) { return 1; }
EOF
cat > "$group_tmp/group/sys/kern/mm/mm.c" <<'EOF'
int kernel_mm_value(void) { return 2; }
EOF
cat > "$group_tmp/group/sys/kern/irq/irq.c" <<'EOF'
int kernel_irq_value(void) { return 3; }
EOF
"$qstar" --file "$group_tmp/group/qstar.lua" check //:firmware_image > "$tmp/group-check.out" 2> "$tmp/group-check.err"
contains "$tmp/group-check.out" "status ok"
"$qstar" --file "$group_tmp/group/qstar.lua" dry-run //:firmware_image > "$tmp/group-dry.out" 2> "$tmp/group-dry.err"
contains "$tmp/group-dry.out" "dry_run_target //sys/kern:kernel_subsystems"
contains "$tmp/group-dry.out" "kind=group tool=none input=<deps> output=<none>"
contains "$tmp/group-dry.out" "progress_action label=//sys/kern:kernel_subsystems include=no reason=group"
contains "$tmp/group-dry.out" "progress_action label=//:firmware_image include=no reason=group"
"$qstar" --file "$group_tmp/group/qstar.lua" build //:firmware_image --schedule-trace --progress off > "$tmp/group-trace.out" 2> "$tmp/group-trace.err"
contains "$tmp/group-trace.out" "action_scheduler version=v1"
contains "$tmp/group-trace.out" "ready=3 jobs="
not_contains "$tmp/group-trace.out" "ready=1 jobs="
contains "$tmp/group-trace.out" "schedule_action id=//lib/libk:libk_core:compile:0"
contains "$tmp/group-trace.out" "schedule_action id=//sys/kern/mm:kernel_mm:compile:0"
contains "$tmp/group-trace.out" "schedule_action id=//sys/kern/irq:kernel_irq:compile:0"
contains "$tmp/group-trace.out" "schedule_action id=//lib/libk:libk_core:archive:0 kind=archive slot="
contains "$tmp/group-trace.out" "schedule_action id=//sys/kern/mm:kernel_mm:archive:0 kind=archive slot="
contains "$tmp/group-trace.out" "schedule_action id=//sys/kern/irq:kernel_irq:archive:0 kind=archive slot="
not_contains "$tmp/group-trace.out" "kind=final state=ready"
"$qstar" --file "$group_tmp/group/qstar.lua" build //:firmware_image > "$tmp/group-build.out" 2> "$tmp/group-build.err"
contains "$tmp/group-build.out" "group_target label=//sys/kern:kernel_subsystems"
contains "$tmp/group-build.out" "group_target label=//:firmware_image"
contains "$tmp/group-build.out" "artifact=<none>"
contains "$tmp/group-build.out" "status ok"
"$qstar" --file "$group_tmp/group/qstar.lua" list-targets --format json > "$tmp/group-targets-json.out" 2> "$tmp/group-targets-json.err"
contains "$tmp/group-targets-json.out" "\"kind\":\"group\""
contains "$tmp/group-targets-json.out" "\"installable\":false"

step "noop run target optimization" "noop-run-build"
mkdir -p "$group_tmp/noop-run/src"
cat > "$group_tmp/noop-run/qstar.lua" <<'EOF'
qstar.project {
  name = "noop-run-corpus",
  version = "0.1.0",
  root = ".",
}

qstar.staticlib "core" {
  sources = {"src/core.c"},
}

qstar.run_target "all" {
  deps = {"//:core"},
  command = qstar.cli {"true"},
}
EOF
cat > "$group_tmp/noop-run/src/core.c" <<'EOF'
int core(void) { return 4; }
EOF
"$qstar" --file "$group_tmp/noop-run/qstar.lua" build //:all --progress off > "$tmp/noop-run-build.out" 2> "$tmp/noop-run-build.err"
contains "$tmp/noop-run-build.out" "status ok"
not_contains "$tmp/noop-run-build.out" "run_target label=//:all command=argv"
test ! -f "$group_tmp/noop-run/build/qstar/out/___all/run.stamp" || fail "noop run_target wrote stamp"
if command -v ninja >/dev/null 2>&1; then
	"$qstar" --file "$group_tmp/noop-run/qstar.lua" -G ninja build //:all --progress off > "$tmp/noop-run-ninja.out" 2> "$tmp/noop-run-ninja.err"
	contains "$tmp/noop-run-ninja.out" "backend ninja"
	contains "$tmp/noop-run-ninja.out" "status ok"
	contains "$group_tmp/noop-run/build/qstar/ninja/build.ninja" "qstar_action_id = //:all:run:0"
	test ! -f "$group_tmp/noop-run/build/qstar/out/___all/run.stamp" || fail "ninja noop run_target wrote stamp"
	test ! -f "$group_tmp/noop-run/.ninja_log" || fail "ninja wrote root .ninja_log"
	test ! -f "$group_tmp/noop-run/.ninja_deps" || fail "ninja wrote root .ninja_deps"
fi

step "group target_file diagnostic" "group-bad"
mkdir -p "$group_tmp/group-bad"
cat > "$group_tmp/group-bad/qstar.lua" <<'EOF'
qstar.group "bundle" {
  deps = {},
}

qstar.run_target "bad" {
  deps = {
    "//:bundle",
  },
  command = qstar.cli {
    qstar.target_file("//:bundle"),
  },
}
EOF
if "$qstar" --file "$group_tmp/group-bad/qstar.lua" check //:bad > "$tmp/group-bad.out" 2> "$tmp/group-bad.err"; then
	fail "target_file(group) unexpectedly succeeded"
fi
contains "$tmp/group-bad.err" "qstar.target_file cannot reference group target '//:bundle' because group targets have no artifact"

step "compile_commands root policy" "build-policy-root"
mkdir -p "$tmp/build-policy-root/src"
cat > "$tmp/build-policy-root/qstar.lua" <<'EOF'
qstar.project {
  name = "build-policy-root",
  version = "0.1.0",
  root = ".",
  build_dir = "out/qstar",
  compile_commands = "root",
}

qstar.executable "app" {
  sources = {"src/main.c"},
}
EOF
cp "$tmp/src/main.c" "$tmp/build-policy-root/src/main.c"
cp "$tmp/src/dep.h" "$tmp/build-policy-root/src/dep.h"
"$qstar" --file "$tmp/build-policy-root/qstar.lua" build //:app > "$tmp/build-policy-root.out" 2> "$tmp/build-policy-root.err"
test -f "$tmp/build-policy-root/out/qstar/state/state.db" || fail "custom build_dir compact state missing"
test -f "$tmp/build-policy-root/out/qstar/state/deps.db" || fail "custom build_dir compact deps missing"
test ! -e "$tmp/build-policy-root/out/qstar/state/actions.json" || fail "custom build_dir debug state dump should be opt-in"
test -f "$tmp/build-policy-root/compile_commands.json" || fail "root compile_commands policy missing"
test ! -f "$tmp/build-policy-root/out/qstar/compile_commands.json" || fail "root compile_commands policy wrote build db"

step "compile_commands off policy" "build-policy-off"
mkdir -p "$tmp/build-policy-off/src"
cat > "$tmp/build-policy-off/qstar.lua" <<'EOF'
qstar.project {
  name = "build-policy-off",
  version = "0.1.0",
  root = ".",
  compile_commands = "off",
}

qstar.executable "app" {
  sources = {"src/main.c"},
}
EOF
cp "$tmp/src/main.c" "$tmp/build-policy-off/src/main.c"
cp "$tmp/src/dep.h" "$tmp/build-policy-off/src/dep.h"
"$qstar" --file "$tmp/build-policy-off/qstar.lua" build //:app > "$tmp/build-policy-off.out" 2> "$tmp/build-policy-off.err"
test -f "$tmp/build-policy-off/build/qstar/state/state.db" || fail "off policy compact state missing"
test -f "$tmp/build-policy-off/build/qstar/state/deps.db" || fail "off policy compact deps missing"
test ! -e "$tmp/build-policy-off/build/qstar/state/actions.json" || fail "off policy debug state dump should be opt-in"
test ! -f "$tmp/build-policy-off/build/qstar/compile_commands.json" || fail "off policy wrote build compile_commands"
test ! -f "$tmp/build-policy-off/compile_commands.json" || fail "off policy wrote root compile_commands"

step "CLI generator and build-dir overrides" "cli-overrides-build"
mkdir -p "$tmp/cli-overrides/src"
cat > "$tmp/cli-overrides/qstar.lua" <<'EOF'
qstar.project {
  name = "cli-overrides",
  version = "0.1.0",
  root = ".",
  build_dir = "project/qstar",
}

qstar.executable "app" {
  sources = {"src/main.c"},
}
EOF
cp "$tmp/src/main.c" "$tmp/cli-overrides/src/main.c"
cp "$tmp/src/dep.h" "$tmp/cli-overrides/src/dep.h"
"$qstar" --file "$tmp/cli-overrides/qstar.lua" -B cli/qstar -G auto build //:app > "$tmp/cli-overrides-build.out" 2> "$tmp/cli-overrides-build.err"
test -f "$tmp/cli-overrides/cli/qstar/state/state.db" || fail "CLI -B build dir compact state missing"
test -f "$tmp/cli-overrides/cli/qstar/state/deps.db" || fail "CLI -B build dir compact deps missing"
test ! -e "$tmp/cli-overrides/cli/qstar/state/actions.json" || fail "CLI -B debug state dump should be opt-in"
test ! -e "$tmp/cli-overrides/project/qstar/state/state.db" || fail "qstar.project build_dir overrode CLI -B"
test -f "$tmp/cli-overrides/cli/qstar/compile_commands.json" || fail "CLI -B compile_commands missing"
test ! -e "$tmp/cli-overrides/cli/qstar/state/graph.json" || fail "CLI -B graph snapshot should be debug opt-in"
QSTAR_DEBUG_STATE_DUMPS=1 "$qstar" --file "$tmp/cli-overrides/qstar.lua" -B cli/qstar -G auto build //:app --progress off > "$tmp/cli-overrides-debug-state.out" 2> "$tmp/cli-overrides-debug-state.err"
contains "$tmp/cli-overrides/cli/qstar/state/graph.json" "\"build_dir\":\"cli/qstar\""
contains "$tmp/cli-overrides/cli/qstar/state/graph.json" "\"generator\":\"stella\""
contains "$tmp/cli-overrides/cli/qstar/state/graph.json" "\"requested_generator\":\"auto\""
"$qstar" --file "$tmp/cli-overrides/qstar.lua" -B cli/list -G auto list-targets --format json > "$tmp/cli-overrides-json.out" 2> "$tmp/cli-overrides-json.err"
contains "$tmp/cli-overrides-json.out" "\"build_dir\":\"cli/list\""
contains "$tmp/cli-overrides-json.out" "\"generator\":\"stella\""
contains "$tmp/cli-overrides-json.out" "\"requested_generator\":\"auto\""
"$qstar" --file "$tmp/cli-overrides/qstar.lua" -G stella list-targets --format json > "$tmp/cli-overrides-stella-json.out" 2> "$tmp/cli-overrides-stella-json.err"
contains "$tmp/cli-overrides-stella-json.out" "\"generator\":\"stella\""
contains "$tmp/cli-overrides-stella-json.out" "\"requested_generator\":\"stella\""
"$qstar" --file "$tmp/cli-overrides/qstar.lua" --generator ninja list-targets --format json > "$tmp/cli-overrides-ninja-json.out" 2> "$tmp/cli-overrides-ninja-json.err"
contains "$tmp/cli-overrides-ninja-json.out" "\"generator\":\"ninja\""
contains "$tmp/cli-overrides-ninja-json.out" "\"requested_generator\":\"ninja\""
if command -v ninja >/dev/null 2>&1; then
	"$qstar" --file "$tmp/cli-overrides/qstar.lua" -G ninja build //:app > "$tmp/cli-overrides-ninja-build.out" 2> "$tmp/cli-overrides-ninja-build.err"
	contains "$tmp/cli-overrides-ninja-build.out" "backend ninja"
	contains "$tmp/cli-overrides-ninja-build.out" "status ok"
	test -f "$tmp/cli-overrides/project/qstar/out/___app/app" || fail "ninja executable artifact missing"
else
	"$qstar" --file "$tmp/cli-overrides/qstar.lua" emit-ninja //:app > "$tmp/cli-overrides-ninja-build.out" 2> "$tmp/cli-overrides-ninja-build.err"
	contains "$tmp/cli-overrides/project/qstar/ninja/build.ninja" "rule qstar_link"
fi
if "$qstar" --file "$tmp/cli-overrides/qstar.lua" -G nope list-targets --format json > "$tmp/cli-overrides-bad-generator.out" 2> "$tmp/cli-overrides-bad-generator.err"; then
	fail "invalid generator unexpectedly succeeded"
fi
contains "$tmp/cli-overrides-bad-generator.err" "invalid generator 'nope'; expected stella, ninja, or auto"
if "$qstar" --file "$tmp/cli-overrides/qstar.lua" -G qstar_graph list-targets --format json > "$tmp/cli-overrides-qstar-graph.out" 2> "$tmp/cli-overrides-qstar-graph.err"; then
	fail "qstar_graph generator unexpectedly succeeded"
fi
contains "$tmp/cli-overrides-qstar-graph.err" "invalid generator 'qstar_graph'; expected stella, ninja, or auto"
if "$qstar" --file "$tmp/cli-overrides/qstar.lua" -B /tmp/qstar-build list-targets --format json > "$tmp/cli-overrides-bad-builddir.out" 2> "$tmp/cli-overrides-bad-builddir.err"; then
	fail "absolute CLI build directory unexpectedly succeeded"
fi
contains "$tmp/cli-overrides-bad-builddir.err" "CLI build directory override must be package-relative"

step "Ninja MVP lowering" "ninja-mvp-emit"
mkdir -p "$tmp/ninja-mvp/src" "$tmp/ninja-mvp/include"
cat > "$tmp/ninja-mvp/qstar.lua" <<'EOF'
qstar.project {
  name = "ninja-mvp",
  root = ".",
  build_dir = "build/qstar",
  compile_commands = "build",
}

qstar.staticlib "core" {
  sources = {
    "src/core.c",
    "src/helper.cc",
    "src/start.S",
  },
  lang = {
    c = {
      public_headers = {"include/core.h"},
      public_include_dirs = {"include"},
      compile_options = {"-Wall"},
    },
    cxx = {
      standard = "c++11",
      compile_options = {"-Wall"},
    },
  },
}

qstar.group "all" {
  deps = {"//:core"},
}
EOF
cat > "$tmp/ninja-mvp/include/core.h" <<'EOF'
int core(void);
EOF
cat > "$tmp/ninja-mvp/src/core.c" <<'EOF'
#include "core.h"
int core(void) { return 1; }
EOF
cat > "$tmp/ninja-mvp/src/helper.cc" <<'EOF'
extern "C" int helper(void) { return 2; }
EOF
cat > "$tmp/ninja-mvp/src/start.S" <<'EOF'
/* intentionally empty assembler input for backend smoke */
EOF
"$qstar" --file "$tmp/ninja-mvp/qstar.lua" emit-ninja //:all > "$tmp/ninja-mvp-emit.out" 2> "$tmp/ninja-mvp-emit.err"
contains "$tmp/ninja-mvp-emit.out" "ninja_file build/qstar/ninja/build.ninja"
contains "$tmp/ninja-mvp-emit.out" "compile_commands build/qstar/compile_commands.json"
contains "$tmp/ninja-mvp-emit.out" "ninja_default build/qstar/ninja/targets/___all"
"$qstar" --file "$tmp/ninja-mvp/qstar.lua" dry-run //:all > "$tmp/ninja-mvp-dry.out" 2> "$tmp/ninja-mvp-dry.err"
contains "$tmp/ninja-mvp-dry.out" "action_description id=//:core:compile:0 text=\"Building C object build/qstar/out/___core/obj0.o\""
contains "$tmp/ninja-mvp-dry.out" "action_description id=//:core:compile:1 text=\"Building CXX object build/qstar/out/___core/obj1.o\""
contains "$tmp/ninja-mvp-dry.out" "action_description id=//:core:compile:2 text=\"Building ASM object build/qstar/out/___core/obj2.o\""
contains "$tmp/ninja-mvp-dry.out" "action_description id=//:core:archive:0 text=\"Linking CXX static library build/qstar/out/___core/libcore.a\""
contains "$tmp/ninja-mvp-dry.out" "progress_action label=//:all include=no reason=group"
contains "$tmp/ninja-mvp/build/qstar/ninja/build.ninja" "rule qstar_compile"
contains "$tmp/ninja-mvp/build/qstar/ninja/build.ninja" "builddir = build/qstar/ninja"
contains "$tmp/ninja-mvp/build/qstar/ninja/build.ninja" "rule qstar_archive"
contains "$tmp/ninja-mvp/build/qstar/ninja/build.ninja" "build build/qstar/out/___core/libcore.a: qstar_archive"
contains "$tmp/ninja-mvp/build/qstar/ninja/build.ninja" "qstar_action_id = //:core:compile:0"
contains "$tmp/ninja-mvp/build/qstar/ninja/build.ninja" "qstar_action_id = //:core:compile:1"
contains "$tmp/ninja-mvp/build/qstar/ninja/build.ninja" "qstar_action_id = //:core:compile:2"
contains "$tmp/ninja-mvp/build/qstar/ninja/build.ninja" "qstar_action_id = //:core:archive:0"
contains "$tmp/ninja-mvp/build/qstar/ninja/build.ninja" "qstar_action_id = //:all:group:0"
contains "$tmp/ninja-mvp/build/qstar/compile_commands.json" "src/core.c"
contains "$tmp/ninja-mvp/build/qstar/compile_commands.json" "src/helper.cc"
contains "$tmp/ninja-mvp/build/qstar/compile_commands.json" "src/start.S"
"$qstar" --file "$tmp/ninja-mvp/qstar.lua" action-log //:core:archive:0 > "$tmp/ninja-mvp-archive-log.out" 2> "$tmp/ninja-mvp-archive-log.err"
contains "$tmp/ninja-mvp-archive-log.out" "qstar action-log v1"
contains "$tmp/ninja-mvp-archive-log.out" "backend=ninja"
contains "$tmp/ninja-mvp-archive-log.out" "argv[0]=ar"
contains "$tmp/ninja-mvp-archive-log.out" "libcore.a"
"$qstar" --file "$tmp/ninja-mvp/qstar.lua" replay //:core:archive:0 > "$tmp/ninja-mvp-archive-replay.out" 2> "$tmp/ninja-mvp-archive-replay.err"
contains "$tmp/ninja-mvp-archive-replay.out" "qstar replay v1"
contains "$tmp/ninja-mvp-archive-replay.out" "libcore.a"
if command -v ninja >/dev/null 2>&1 && command -v c++ >/dev/null 2>&1; then
	"$qstar" --file "$tmp/ninja-mvp/qstar.lua" -G ninja build //:all > "$tmp/ninja-mvp-build.out" 2> "$tmp/ninja-mvp-build.err"
	contains "$tmp/ninja-mvp-build.out" "backend ninja"
	contains "$tmp/ninja-mvp-build.out" "status ok"
	test -f "$tmp/ninja-mvp/build/qstar/out/___core/libcore.a" || fail "ninja backend staticlib artifact missing"
	test ! -f "$tmp/ninja-mvp/.ninja_log" || fail "ninja wrote root .ninja_log"
	test ! -f "$tmp/ninja-mvp/.ninja_deps" || fail "ninja wrote root .ninja_deps"
fi

step "Ninja backend parity surface" "ninja-parity-emit"
mkdir -p "$tmp/ninja-parity/src" "$tmp/ninja-parity/tools"
cat > "$tmp/ninja-parity/tools/gen-value.sh" <<'EOF'
#!/bin/sh
set -eu
config=$1
out=$2
test -f "$config"
cat > "$out" <<'GEN'
#include "config.h"
int generated_value(void) { return APP_VALUE; }
GEN
EOF
chmod +x "$tmp/ninja-parity/tools/gen-value.sh"
cat > "$tmp/ninja-parity/qstar.lua" <<'EOF'
local long_flags = {}
for i = 1, 96 do
  table.insert(long_flags, "-DQSTAR_NINJA_RSP_" .. i .. "=" .. i)
end

qstar.project {
  name = "ninja-parity",
  root = ".",
  build_dir = "build/qstar",
  compile_commands = "build",
}

qstar.profile "ninja-parity" {
  response_files = "on",
  response_style = "posix",
  tool_overrides = {"qstar-gen-value=tools/gen-value.sh"},
  compile_options = long_flags,
}

qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"APP_VALUE=42", "APP_FEATURE"},
}

qstar.custom_target "make_value" {
  inputs = {qstar.target_file("//:cfg")},
  outputs = {qstar.output("generated/value.c")},
  command = qstar.cli {"qstar-gen-value", qstar.target_file("//:cfg"), qstar.output(0)},
}

qstar.staticlib "core" {
  sources = {qstar.output("generated/value.c")},
  lang = {
    c = {
      private_headers = {qstar.output("generated/config.h")},
      include_dirs = {"generated"},
    },
  },
}

qstar.executable "app" {
  sources = {"src/main.c"},
  deps = {"//:core"},
  lang = {
    c = {
      include_dirs = {"generated"},
    },
  },
}

qstar.test "unit" {
  sources = {"src/test.c"},
  deps = {"//:core"},
  lang = {
    c = {
      include_dirs = {"generated"},
    },
  },
}

qstar.run_target "smoke" {
  deps = {"//:app"},
  command = qstar.cli {qstar.target_file("//:app")},
  timeout = 3,
  marker = "APP-OK",
}

qstar.run_target "missing_marker" {
  deps = {"//:app"},
  command = qstar.cli {qstar.target_file("//:app")},
  timeout = 3,
  marker = "MISSING-MARKER",
}

qstar.stage "bundle" {
  root = "stage/bundle",
  files = {
    qstar.stage_file(qstar.target_file("//:app"), "bin/app"),
  },
}
EOF
cat > "$tmp/ninja-parity/src/main.c" <<'EOF'
#include <stdio.h>
int generated_value(void);
int main(void) {
  puts("APP-OK");
  return generated_value() == 42 ? 0 : 1;
}
EOF
cat > "$tmp/ninja-parity/src/test.c" <<'EOF'
int generated_value(void);
int main(void) { return generated_value() == 42 ? 0 : 1; }
EOF
"$qstar" --file "$tmp/ninja-parity/qstar.lua" --profile ninja-parity emit-ninja //:app > "$tmp/ninja-parity-emit.out" 2> "$tmp/ninja-parity-emit.err"
contains "$tmp/ninja-parity/build/qstar/ninja/build.ninja" "rule qstar_generate"
contains "$tmp/ninja-parity/build/qstar/ninja/build.ninja" "rule qstar_link"
contains "$tmp/ninja-parity/build/qstar/ninja/build.ninja" "qstar_action_id = //:cfg:generate:0"
contains "$tmp/ninja-parity/build/qstar/ninja/build.ninja" "qstar_action_id = //:make_value:generate:0"
contains "$tmp/ninja-parity/build/qstar/ninja/build.ninja" "qstar_action_id = //:app:link:0"
contains "$tmp/ninja-parity/build/qstar/logs/___app_link_0.log" "backend=ninja"
contains "$tmp/ninja-parity/build/qstar/logs/___app_compile_0.log" "response_file=build/qstar/rsp/"
"$qstar" --file "$tmp/ninja-parity/qstar.lua" --profile ninja-parity action-log //:app:link:0 > "$tmp/ninja-parity-action-log.out" 2> "$tmp/ninja-parity-action-log.err"
contains "$tmp/ninja-parity-action-log.out" "qstar action-log v1"
contains "$tmp/ninja-parity-action-log.out" "backend=ninja"
"$qstar" --file "$tmp/ninja-parity/qstar.lua" --profile ninja-parity action-log //:make_value:generate:0 > "$tmp/ninja-parity-generate-log.out" 2> "$tmp/ninja-parity-generate-log.err"
contains "$tmp/ninja-parity-generate-log.out" "qstar action-log v1"
contains "$tmp/ninja-parity-generate-log.out" "backend=ninja"
contains "$tmp/ninja-parity-generate-log.out" "tools/gen-value.sh"
contains "$tmp/ninja-parity-generate-log.out" "generated/value.c"
"$qstar" --file "$tmp/ninja-parity/qstar.lua" --profile ninja-parity replay //:app:link:0 > "$tmp/ninja-parity-replay.out" 2> "$tmp/ninja-parity-replay.err"
contains "$tmp/ninja-parity-replay.out" "qstar replay v1"
contains "$tmp/ninja-parity-replay.out" "build/qstar/out/___app/app"
if command -v ninja >/dev/null 2>&1; then
	"$qstar" --file "$tmp/ninja-parity/qstar.lua" --profile ninja-parity -G ninja build //:app > "$tmp/ninja-parity-build.out" 2> "$tmp/ninja-parity-build.err"
	contains "$tmp/ninja-parity-build.out" "backend ninja"
	contains "$tmp/ninja-parity-build.out" "status ok"
	test -f "$tmp/ninja-parity/build/qstar/out/___app/app" || fail "ninja parity executable artifact missing"
	"$qstar" --file "$tmp/ninja-parity/qstar.lua" --profile ninja-parity -G ninja test //:unit > "$tmp/ninja-parity-test.out" 2> "$tmp/ninja-parity-test.err"
	contains "$tmp/ninja-parity-test.out" "backend ninja"
	contains "$tmp/ninja-parity-test.out" "test_result label=//:unit status=pass"
	"$qstar" --file "$tmp/ninja-parity/qstar.lua" --profile ninja-parity -G ninja build //:smoke > "$tmp/ninja-parity-run.out" 2> "$tmp/ninja-parity-run.err"
	contains "$tmp/ninja-parity-run.out" "run_target label=//:smoke"
	contains "$tmp/ninja-parity-run.out" "run_marker label=//:smoke status=matched"
	contains "$tmp/ninja-parity-run.out" "run_target_result label=//:smoke status=pass"
	"$qstar" --file "$tmp/ninja-parity/qstar.lua" --profile ninja-parity action-log //:smoke:run:0 > "$tmp/ninja-parity-run-log.out" 2> "$tmp/ninja-parity-run-log.err"
	contains "$tmp/ninja-parity-run-log.out" "qstar action-log v1"
	contains "$tmp/ninja-parity-run-log.out" "backend=ninja"
	contains "$tmp/ninja-parity-run-log.out" "build/qstar/out/___app/app"
	if "$qstar" --file "$tmp/ninja-parity/qstar.lua" --profile ninja-parity -G ninja build //:missing_marker > "$tmp/ninja-parity-run-missing.out" 2> "$tmp/ninja-parity-run-missing.err"; then
		fail "ninja marker-missing run_target unexpectedly succeeded"
	fi
	cat "$tmp/ninja-parity-run-missing.out" "$tmp/ninja-parity-run-missing.err" > "$tmp/ninja-parity-run-missing.combined"
	contains "$tmp/ninja-parity-run-missing.combined" "run_target_result label=//:missing_marker status=marker-missing"
	contains "$tmp/ninja-parity/build/qstar/logs/last-failure.replay" "failure_kind=marker-missing"
	"$qstar" --file "$tmp/ninja-parity/qstar.lua" --profile ninja-parity last-failure > "$tmp/ninja-parity-last-failure.out" 2> "$tmp/ninja-parity-last-failure.err"
	contains "$tmp/ninja-parity-last-failure.out" "qstar last-failure v1"
	contains "$tmp/ninja-parity-last-failure.out" "label=//:missing_marker"
	contains "$tmp/ninja-parity-last-failure.out" "failure_kind=marker-missing"
	"$qstar" --file "$tmp/ninja-parity/qstar.lua" --profile ninja-parity -G ninja stage //:bundle > "$tmp/ninja-parity-stage.out" 2> "$tmp/ninja-parity-stage.err"
	contains "$tmp/ninja-parity-stage.out" "backend ninja"
	contains "$tmp/ninja-parity-stage.out" "stage_file src=build/qstar/out/___app/app dst=stage/bundle/bin/app mode=copy"
	test -f "$tmp/ninja-parity/stage/bundle/bin/app" || fail "ninja stage output missing"
	contains "$tmp/ninja-parity/build/qstar/stage/___bundle/manifest.json" "\"schema\":\"qstar-stage-manifest-v2\""
	contains "$tmp/ninja-parity/build/qstar/stage/___bundle/manifest.json" "\"producer\":\"//:app\""
	"$qstar" --file "$tmp/ninja-parity/qstar.lua" --profile ninja-parity -G ninja install //:app --prefix "$tmp/ninja-parity-prefix" > "$tmp/ninja-parity-install.out" 2> "$tmp/ninja-parity-install.err"
	contains "$tmp/ninja-parity-install.out" "backend ninja"
	contains "$tmp/ninja-parity-install.out" "install_file src=build/qstar/out/___app/app"
	test -f "$tmp/ninja-parity-prefix/bin/app" || fail "ninja install output missing"
	contains "$tmp/ninja-parity/build/qstar/install/manifest.json" "\"schema\":\"qstar-install-manifest-v2\""
	contains "$tmp/ninja-parity/build/qstar/install/manifest.json" "\"role\":\"exe\""
fi

step "Ninja compile_commands root policy" "ninja-policy-root"
mkdir -p "$tmp/ninja-policy-root/src"
cat > "$tmp/ninja-policy-root/qstar.lua" <<'EOF'
qstar.project {
  name = "ninja-policy-root",
  root = ".",
  build_dir = "out/qstar",
  compile_commands = "root",
}

qstar.staticlib "core" {
  sources = {"src/core.c"},
}
EOF
cat > "$tmp/ninja-policy-root/src/core.c" <<'EOF'
int root_policy(void) { return 0; }
EOF
"$qstar" --file "$tmp/ninja-policy-root/qstar.lua" emit-ninja //:core > "$tmp/ninja-policy-root.out" 2> "$tmp/ninja-policy-root.err"
contains "$tmp/ninja-policy-root.out" "compile_commands compile_commands.json"
test -f "$tmp/ninja-policy-root/compile_commands.json" || fail "ninja root compile_commands policy missing"
test ! -f "$tmp/ninja-policy-root/out/qstar/compile_commands.json" || fail "ninja root policy wrote build db"

step "Ninja compile_commands off policy" "ninja-policy-off"
mkdir -p "$tmp/ninja-policy-off/src"
cat > "$tmp/ninja-policy-off/qstar.lua" <<'EOF'
qstar.project {
  name = "ninja-policy-off",
  root = ".",
  compile_commands = "off",
}

qstar.staticlib "core" {
  sources = {"src/core.c"},
}
EOF
cat > "$tmp/ninja-policy-off/src/core.c" <<'EOF'
int off_policy(void) { return 0; }
EOF
"$qstar" --file "$tmp/ninja-policy-off/qstar.lua" emit-ninja //:core > "$tmp/ninja-policy-off.out" 2> "$tmp/ninja-policy-off.err"
contains "$tmp/ninja-policy-off.out" "compile_commands <off>"
test ! -f "$tmp/ninja-policy-off/build/qstar/compile_commands.json" || fail "ninja off policy wrote build db"
test ! -f "$tmp/ninja-policy-off/compile_commands.json" || fail "ninja off policy wrote root db"

step "version command surface" "version-flag"
"$qstar" --version > "$tmp/version-flag.out" 2> "$tmp/version-flag.err"
test "$(cat "$tmp/version-flag.out")" = "qstar 0.6.1-beta" || fail "qstar --version drifted"
"$qstar" version > "$tmp/version-cmd.out" 2> "$tmp/version-cmd.err"
test "$(cat "$tmp/version-cmd.out")" = "qstar 0.6.1-beta" || fail "qstar version drifted"

"$qstar" --file "$tmp/qstar.lua" lint > "$tmp/lint-ok.out" 2> "$tmp/lint-ok.err"
contains "$tmp/lint-ok.out" "qstar lint v1"
contains "$tmp/lint-ok.out" "summary errors=0 warnings=0"
contains "$tmp/lint-ok.out" "status ok"
"$qstar" --file "$tmp/qstar.lua" lint --format json > "$tmp/lint-json.out" 2> "$tmp/lint-json.err"
contains "$tmp/lint-json.out" "\"schema\":\"qstar-lint-v1\""
contains "$tmp/lint-json.out" "\"diagnostics\":[]"
"$qstar" --file "$tmp/qstar.lua" list-targets --format json > "$tmp/targets-json.out" 2> "$tmp/targets-json.err"
contains "$tmp/targets-json.out" "\"schema\":\"qstar-targets-v1\""
contains "$tmp/targets-json.out" "\"project\":{\"name\":\"smoke\""
contains "$tmp/targets-json.out" "\"target_count\":3"
contains "$tmp/targets-json.out" "\"generated_action_count\":0"
contains "$tmp/targets-json.out" "\"label\":\"//:app\""
contains "$tmp/targets-json.out" "\"label\":\"//:smoke\""
contains "$tmp/targets-json.out" "\"is_test\":false"
contains "$tmp/targets-json.out" "\"installable\":true"

step "project root validation" "project-root-reject"
mkdir -p "$tmp/project-root-reject"
cat > "$tmp/project-root-reject/qstar.lua" <<'EOF'
qstar.project {
  name = "bad-root",
  root = "src",
}
EOF
if "$qstar" --file "$tmp/project-root-reject/qstar.lua" lint > "$tmp/project-root-reject.out" 2> "$tmp/project-root-reject.err"; then
	fail "qstar.project non-dot root unexpectedly succeeded"
fi
contains "$tmp/project-root-reject.out" "qstar.project root must be \".\" in v1"

step "Lua authoring helpers" "lua-authoring"
mkdir -p "$tmp/lua-authoring/include" "$tmp/lua-authoring/src"
cat > "$tmp/lua-authoring/src/core.c" <<'EOF'
int core(void) { return 0; }
EOF
cat > "$tmp/lua-authoring/qstar.lua" <<'EOF'
qstar.project {
  name = "lua-authoring",
  version = "0.1.0",
  root = ".",
}

local function common_c()
  local opts = {}
  local count = 0
  for _, flag in ipairs({"-Wall", "-Wextra"}) do
    table.insert(opts, flag)
  end
  for _ in pairs({one = 1, two = 2}) do
    count = count + 1
  end
  table.insert(opts, string.upper("-dqstar_profile=" .. QSTAR_PROFILE))
  table.insert(opts, "-DQSTAR_VERSION=" .. qstar.version)
  table.insert(opts, "-DQSTAR_VERSION_MINOR=" .. QSTAR_VERSION_MINOR)
  table.insert(opts, "-DQSTAR_HOST_OS=" .. qstar.host.os)
  table.insert(opts, "-DQSTAR_HOST_ARCH=" .. QSTAR_HOST_ARCH)
  table.insert(opts, "-DQSTAR_PACKAGE_ROOT=" .. QSTAR_PACKAGE_ROOT)
  table.insert(opts, "-DQSTAR_PROJECT_ROOT=" .. QSTAR_PROJECT_ROOT)
  table.insert(opts, "-DQSTAR_PROJECT_NS_ROOT=" .. qstar.project.root)
  table.insert(opts, "-DQSTAR_TARGET=" .. QSTAR_TARGET)
  table.insert(opts, "-DQSTAR_PAIR_COUNT=" .. count)
  return {
    public_include_dirs = {"include"},
    compile_options = opts,
  }
end

qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = common_c(),
  },
}
EOF
"$qstar" --file "$tmp/lua-authoring/qstar.lua" --dump-graph > "$tmp/lua-authoring.out" 2> "$tmp/lua-authoring.err"
contains "$tmp/lua-authoring.out" "cflags [-Wall, -Wextra"
contains "$tmp/lua-authoring.out" "-DQSTAR_PROFILE=DEFAULT"
contains "$tmp/lua-authoring.out" "-DQSTAR_VERSION=0.6.1-beta"
contains "$tmp/lua-authoring.out" "-DQSTAR_VERSION_MINOR=6"
contains "$tmp/lua-authoring.out" "-DQSTAR_HOST_OS="
contains "$tmp/lua-authoring.out" "-DQSTAR_HOST_ARCH="
contains "$tmp/lua-authoring.out" "-DQSTAR_PACKAGE_ROOT="
contains "$tmp/lua-authoring.out" "-DQSTAR_PROJECT_ROOT="
contains "$tmp/lua-authoring.out" "-DQSTAR_PROJECT_NS_ROOT="
contains "$tmp/lua-authoring.out" "-DQSTAR_TARGET=host"
contains "$tmp/lua-authoring.out" "-DQSTAR_PAIR_COUNT=2"

step "import_file and import_module happy path" "imports-graph"
mkdir -p "$tmp/imports/qstar/policies" "$tmp/imports/qstar/modules/kernel" \
	"$tmp/imports/include" "$tmp/imports/sys/include" "$tmp/imports/src"
cat > "$tmp/imports/src/app.c" <<'EOF'
int app(void) { return 0; }
EOF
cat > "$tmp/imports/src/policy.c" <<'EOF'
int policy(void) { return 0; }
EOF
cat > "$tmp/imports/qstar.lua" <<'EOF'
qstar.project {
  name = "imports",
  root = ".",
}

qstar.import_file("qstar/policies/common.qst")
local kernel = qstar.import_module("qstar/modules/kernel")

qstar.staticlib "app" {
  sources = qstar.join {
    {qstar.join("src", "app.c")},
  },
  deps = {"//qstar/policies:policy_core"},
  lang = {
    c = kernel.common_c(),
  },
}
EOF
cat > "$tmp/imports/qstar/policies/common.qst" <<'EOF'
qstar.staticlib "policy_core" {
  sources = {"src/policy.c"},
}
EOF
cat > "$tmp/imports/qstar/modules/kernel/kernel.qsm" <<'EOF'
local M = {}

local base_c = {
  public_include_dirs = {qstar.join("include")},
  compile_options = {"-Wall"},
}

local freestanding_c = {
  public_include_dirs = {qstar.join("sys", "include")},
  compile_options = {"-Wextra"},
}

function M.common_c()
  local opts = qstar.merge(base_c, freestanding_c)
  qstar.extend(opts, {
    compile_options = qstar.append({"-Werror"}),
    defines = {"KERNEL_HELPER=1"},
  })
  local copied = qstar.copy(opts)
  qstar.extend(opts, {
    compile_options = {"-DORIGINAL_ONLY=1"},
  })
  return copied
end

return M
EOF
"$qstar" --file "$tmp/imports/qstar.lua" --dump-graph > "$tmp/imports-graph.out" 2> "$tmp/imports-graph.err"
contains "$tmp/imports-graph.out" "target //qstar/policies:policy_core"
contains "$tmp/imports-graph.out" "target //:app"
contains "$tmp/imports-graph.out" "sources [src/app.c]"
contains "$tmp/imports-graph.out" "public_include_dirs [include, sys/include]"
contains "$tmp/imports-graph.out" "cflags [-Wall, -Wextra, -Werror, -DKERNEL_HELPER=1]"
not_contains "$tmp/imports-graph.out" "ORIGINAL_ONLY"
"$qstar" --file "$tmp/imports/qstar.lua" lint //... > "$tmp/imports-lint.out" 2> "$tmp/imports-lint.err"
contains "$tmp/imports-lint.out" "status ok"
"$qstar" fmt --check "$tmp/imports/qstar/modules/kernel/kernel.qsm" > "$tmp/imports-qsm-fmt.out" 2> "$tmp/imports-qsm-fmt.err"
contains "$tmp/imports-qsm-fmt.out" "status ok"

step "import duplicate file diagnostic" "import-duplicate-file"
mkdir -p "$tmp/import-duplicate-file/p"
cat > "$tmp/import-duplicate-file/qstar.lua" <<'EOF'
qstar.project { name = "dup-file", root = "." }
qstar.import_file("p/common.qst")
qstar.import_file("p/common.qst")
EOF
cat > "$tmp/import-duplicate-file/p/common.qst" <<'EOF'
local imported = true
EOF
if "$qstar" --file "$tmp/import-duplicate-file/qstar.lua" check > "$tmp/import-duplicate-file.out" 2> "$tmp/import-duplicate-file.err"; then
	fail "duplicate import_file unexpectedly succeeded"
fi
contains "$tmp/import-duplicate-file.err" "duplicate import 'p/common.qst'"

step "import duplicate module diagnostic" "import-duplicate-module"
mkdir -p "$tmp/import-duplicate-module/mods/a"
cat > "$tmp/import-duplicate-module/qstar.lua" <<'EOF'
qstar.project { name = "dup-module", root = "." }
local a = qstar.import_module("mods/a")
local b = qstar.import_module("mods/a")
EOF
cat > "$tmp/import-duplicate-module/mods/a/a.qsm" <<'EOF'
return {}
EOF
if "$qstar" --file "$tmp/import-duplicate-module/qstar.lua" check > "$tmp/import-duplicate-module.out" 2> "$tmp/import-duplicate-module.err"; then
	fail "duplicate import_module unexpectedly succeeded"
fi
contains "$tmp/import-duplicate-module.err" "duplicate import 'mods/a/a.qsm'"

step "import missing module diagnostic" "import-missing-module"
mkdir -p "$tmp/import-missing-module"
cat > "$tmp/import-missing-module/qstar.lua" <<'EOF'
qstar.project { name = "missing-module", root = "." }
qstar.import_module("mods/missing")
EOF
if "$qstar" --file "$tmp/import-missing-module/qstar.lua" check > "$tmp/import-missing-module.out" 2> "$tmp/import-missing-module.err"; then
	fail "missing import_module unexpectedly succeeded"
fi
contains "$tmp/import-missing-module.err" "expected module entry 'mods/missing/missing.qsm'"

step "import_module file argument diagnostic" "import-module-file-arg"
mkdir -p "$tmp/import-module-file-arg/mods/a"
cat > "$tmp/import-module-file-arg/qstar.lua" <<'EOF'
qstar.project { name = "module-file-arg", root = "." }
qstar.import_module("mods/a/a.qsm")
EOF
if "$qstar" --file "$tmp/import-module-file-arg/qstar.lua" check > "$tmp/import-module-file-arg.out" 2> "$tmp/import-module-file-arg.err"; then
	fail "file-path import_module unexpectedly succeeded"
fi
contains "$tmp/import-module-file-arg.err" "import_module expects a folder path"
contains "$tmp/import-module-file-arg.err" "use qstar.import_module(\"mods/a\")"

step "import_module flat file diagnostic" "import-module-flat-file"
mkdir -p "$tmp/import-module-flat-file"
cat > "$tmp/import-module-flat-file/qstar.lua" <<'EOF'
qstar.project { name = "module-flat-file-arg", root = "." }
qstar.import_module("foo.qsm")
EOF
if "$qstar" --file "$tmp/import-module-flat-file/qstar.lua" check > "$tmp/import-module-flat-file.out" 2> "$tmp/import-module-flat-file.err"; then
	fail "flat file-path import_module unexpectedly succeeded"
fi
contains "$tmp/import-module-flat-file.err" "import_module expects a folder path"
contains "$tmp/import-module-flat-file.err" "use qstar.import_module(\"foo\")"

step "qsm return table diagnostic" "import-module-return"
mkdir -p "$tmp/import-module-return/mods/a"
cat > "$tmp/import-module-return/qstar.lua" <<'EOF'
qstar.project { name = "module-return", root = "." }
qstar.import_module("mods/a")
EOF
cat > "$tmp/import-module-return/mods/a/a.qsm" <<'EOF'
local M = {}
EOF
if "$qstar" --file "$tmp/import-module-return/qstar.lua" check > "$tmp/import-module-return.out" 2> "$tmp/import-module-return.err"; then
	fail "non-returning qsm unexpectedly succeeded"
fi
contains "$tmp/import-module-return.err" "module 'mods/a/a.qsm' must return a table"

step "qsm graph declaration diagnostic" "import-module-graph"
mkdir -p "$tmp/import-module-graph/mods/a"
cat > "$tmp/import-module-graph/qstar.lua" <<'EOF'
qstar.project { name = "module-graph", root = "." }
qstar.import_module("mods/a")
EOF
cat > "$tmp/import-module-graph/mods/a/a.qsm" <<'EOF'
qstar.staticlib "bad" {}
return {}
EOF
if "$qstar" --file "$tmp/import-module-graph/qstar.lua" check > "$tmp/import-module-graph.out" 2> "$tmp/import-module-graph.err"; then
	fail "qsm graph declaration unexpectedly succeeded"
fi
contains "$tmp/import-module-graph.err" "is forbidden inside .qsm module"
contains "$tmp/import-module-graph.err" "modules must return a helper table"

step "circular import diagnostic" "import-circular"
mkdir -p "$tmp/import-circular/mods/a" "$tmp/import-circular/mods/b"
cat > "$tmp/import-circular/qstar.lua" <<'EOF'
qstar.project { name = "circular", root = "." }
qstar.import_module("mods/a")
EOF
cat > "$tmp/import-circular/mods/a/a.qsm" <<'EOF'
qstar.import_module("mods/b")
return {}
EOF
cat > "$tmp/import-circular/mods/b/b.qsm" <<'EOF'
qstar.import_module("mods/a")
return {}
EOF
if "$qstar" --file "$tmp/import-circular/qstar.lua" check > "$tmp/import-circular.out" 2> "$tmp/import-circular.err"; then
	fail "circular import unexpectedly succeeded"
fi
contains "$tmp/import-circular.err" "circular import chain: qstar.lua -> mods/a/a.qsm -> mods/b/b.qsm -> mods/a/a.qsm"

if "$qstar" --file tests/corpus/bad-import/qstar.lua check > "$tmp/bad-import-corpus.out" 2> "$tmp/bad-import-corpus.err"; then
	fail "bad-import corpus unexpectedly succeeded"
fi
contains "$tmp/bad-import-corpus.err" "import_module expects a folder path"
contains "$tmp/bad-import-corpus.err" "use qstar.import_module(\"modules/common\")"

step "config merge corpus" "configs-graph"
mkdir -p "$tmp/configs/qstar/policies" \
	"$tmp/configs/sys/kern/mm" \
	"$tmp/configs/sys/kern/irq" \
	"$tmp/configs/sys/include" \
	"$tmp/configs/lib/libc/aarch64-unknown-none-elf/include"
cat > "$tmp/configs/qstar.lua" <<'EOF'
qstar.project {
  name = "configs",
  root = ".",
}

qstar.import_file("qstar/policies/kernel.qst")
qstar.subdir("sys/kern/mm")
qstar.subdir("sys/kern/irq")
EOF
cat > "$tmp/configs/qstar/policies/kernel.qst" <<'EOF'
qstar.config "kernel_c23" {
  lang = {
    c = {
      public_include_dirs = {"sys/include"},
      system_include_dirs = {"lib/libc/aarch64-unknown-none-elf/include"},
      compile_options = {
        "-std=c23",
        "-ffreestanding",
        "-fno-builtin",
        "-nostdinc",
      },
    },
  },
}

qstar.config "strict_warnings" {
  lang = {
    c = {
      compile_options = {
        "-Wall",
        "-Wextra",
        "-Werror",
      },
    },
  },
}
EOF
cat > "$tmp/configs/sys/kern/mm/mm.qst" <<'EOF'
qstar.staticlib "kernel_mm" {
  configs = {
    "//qstar/policies:kernel_c23",
    "//qstar/policies:strict_warnings",
  },
  sources = {
    "sys/kern/mm/mm.c",
  },
  lang = {
    c = {
      compile_options = {
        "-DMM_LOCAL=1",
      },
    },
  },
}
EOF
cat > "$tmp/configs/sys/kern/irq/irq.qst" <<'EOF'
qstar.staticlib "kernel_irq" {
  configs = {
    "//qstar/policies:kernel_c23",
    "//qstar/policies:strict_warnings",
  },
  sources = {
    "sys/kern/irq/irq.c",
  },
}
EOF
cat > "$tmp/configs/sys/kern/mm/mm.c" <<'EOF'
int kernel_mm_value(void) { return 0; }
EOF
cat > "$tmp/configs/sys/kern/irq/irq.c" <<'EOF'
int kernel_irq_value(void) { return 0; }
EOF
"$qstar" --file "$tmp/configs/qstar.lua" --dump-graph > "$tmp/configs-graph.out" 2> "$tmp/configs-graph.err"
contains "$tmp/configs-graph.out" "config //qstar/policies:kernel_c23"
contains "$tmp/configs-graph.out" "config //qstar/policies:strict_warnings"
contains "$tmp/configs-graph.out" "configs [//qstar/policies:kernel_c23, //qstar/policies:strict_warnings]"
contains "$tmp/configs-graph.out" "public_include_dirs [sys/include]"
contains "$tmp/configs-graph.out" "system_include_dirs [lib/libc/aarch64-unknown-none-elf/include]"
contains "$tmp/configs-graph.out" "cflags [-std=c23, -ffreestanding, -fno-builtin, -nostdinc, -Wall, -Wextra, -Werror, -DMM_LOCAL=1]"
"$qstar" --file "$tmp/configs/qstar.lua" dry-run //sys/kern/mm:kernel_mm > "$tmp/configs-dry.out" 2> "$tmp/configs-dry.err"
contains "$tmp/configs-dry.out" "configs [//qstar/policies:kernel_c23, //qstar/policies:strict_warnings]"
contains "$tmp/configs-dry.out" "-DMM_LOCAL=1"
contains "$tmp/configs-dry.out" "-isystem"
contains "$tmp/configs-dry.out" "lib/libc/aarch64-unknown-none-elf/include"
"$qstar" --file "$tmp/configs/qstar.lua" list-targets --format json > "$tmp/configs-json.out" 2> "$tmp/configs-json.err"
contains "$tmp/configs-json.out" "\"config_count\":2"
contains "$tmp/configs-json.out" "\"configs\":[\"//qstar/policies:kernel_c23\",\"//qstar/policies:strict_warnings\"]"

mkdir -p "$tmp/config-scalar/src"
cat > "$tmp/config-scalar/qstar.lua" <<'EOF'
qstar.config "base" {
  toolchain = "clang",
  stdlib = "freestanding",
  lang = {
    cxx = {
      standard = "c++20",
      modules = {enabled = false},
    },
    asm = {
      preprocess = true,
    },
    cale = {
      profile = "safe",
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:base"},
  toolchain = "host",
  stdlib = "system",
  lang = {
    cxx = {
      standard = "c++23",
    },
  },
}
EOF
"$qstar" --file "$tmp/config-scalar/qstar.lua" --dump-graph > "$tmp/config-scalar.out" 2> "$tmp/config-scalar.err"
contains "$tmp/config-scalar.out" "configs [//:base]"
contains "$tmp/config-scalar.out" "cxx_standard c++23"
contains "$tmp/config-scalar.out" "lang.asm.preprocess true"
contains "$tmp/config-scalar.out" "lang.cale.profile safe"
contains "$tmp/config-scalar.out" "toolchain host"
contains "$tmp/config-scalar.out" "stdlib system"

mkdir -p "$tmp/config-missing"
cat > "$tmp/config-missing/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  configs = {"//:missing"},
}
EOF
if "$qstar" --file "$tmp/config-missing/qstar.lua" check > "$tmp/config-missing.out" 2> "$tmp/config-missing.err"; then
	fail "missing config unexpectedly succeeded"
fi
contains "$tmp/config-missing.err" "references unknown config '//:missing'"

mkdir -p "$tmp/config-conflict"
cat > "$tmp/config-conflict/qstar.lua" <<'EOF'
qstar.config "same" {}
qstar.group "same" {
  deps = {},
}
EOF
if "$qstar" --file "$tmp/config-conflict/qstar.lua" check > "$tmp/config-conflict.out" 2> "$tmp/config-conflict.err"; then
	fail "config/target label conflict unexpectedly succeeded"
fi
contains "$tmp/config-conflict.err" "label '//:same' is already used by config"

mkdir -p "$tmp/config-unknown"
cat > "$tmp/config-unknown/qstar.lua" <<'EOF'
qstar.config "bad" {
  sources = {"src/nope.c"},
}
EOF
if "$qstar" --file "$tmp/config-unknown/qstar.lua" check > "$tmp/config-unknown.out" 2> "$tmp/config-unknown.err"; then
	fail "unknown config field unexpectedly succeeded"
fi
contains "$tmp/config-unknown.err" "unknown config field 'sources'"

mkdir -p "$tmp/config-module/mods/a"
cat > "$tmp/config-module/qstar.lua" <<'EOF'
qstar.import_module("mods/a")
EOF
cat > "$tmp/config-module/mods/a/a.qsm" <<'EOF'
qstar.config "bad" {}
return {}
EOF
if "$qstar" --file "$tmp/config-module/qstar.lua" check > "$tmp/config-module.out" 2> "$tmp/config-module.err"; then
	fail "qsm config declaration unexpectedly succeeded"
fi
contains "$tmp/config-module.err" "qstar.config is forbidden inside .qsm module"

mkdir -p "$tmp/lua-global"
cat > "$tmp/lua-global/qstar.lua" <<'EOF'
qstar.project { name = "global-bad", root = "." }
leaked_global = 1
EOF
if "$qstar" --file "$tmp/lua-global/qstar.lua" check > "$tmp/lua-global.out" 2> "$tmp/lua-global.err"; then
	fail "global assignment unexpectedly succeeded"
fi
contains "$tmp/lua-global.err" "global assignment is not allowed: leaked_global"

mkdir -p "$tmp/lua-forbidden"
forbidden_cases="io.open os.execute require loadfile load dofile debug.getinfo package.loadlib"
for api in $forbidden_cases; do
	cat > "$tmp/lua-forbidden/qstar.lua" <<EOF
qstar.project { name = "forbidden", root = "." }
${api}("x")
EOF
	if "$qstar" --file "$tmp/lua-forbidden/qstar.lua" check > "$tmp/lua-forbidden-$api.out" 2> "$tmp/lua-forbidden-$api.err"; then
		fail "forbidden Lua API $api unexpectedly succeeded"
	fi
	contains "$tmp/lua-forbidden-$api.err" "forbidden Lua API '$api'"
done

mkdir -p "$tmp/fmt"
cat > "$tmp/fmt/qstar.lua" <<'EOF'
qstar.executable "app" {
sources={"src/main.c"},
deps={"//lib:core"},
}
EOF
if "$qstar" fmt --check "$tmp/fmt/qstar.lua" > "$tmp/fmt-check-before.out" 2> "$tmp/fmt-check-before.err"; then
	fail "qstar fmt --check unexpectedly accepted unformatted file"
fi
contains "$tmp/fmt-check-before.out" "qstar fmt v1"
contains "$tmp/fmt-check-before.out" "status needs-format"
"$qstar" fmt --stdout "$tmp/fmt/qstar.lua" > "$tmp/fmt-stdout.out" 2> "$tmp/fmt-stdout.err"
contains "$tmp/fmt-stdout.out" "qstar.executable \"app\" {"
contains "$tmp/fmt-stdout.out" "  sources = {"
contains "$tmp/fmt-stdout.out" "    \"src/main.c\","
contains "$tmp/fmt-stdout.out" "  deps = {"
contains "$tmp/fmt-stdout.out" "    \"//lib:core\","
"$qstar" fmt "$tmp/fmt/qstar.lua" > "$tmp/fmt-write.out" 2> "$tmp/fmt-write.err"
contains "$tmp/fmt-write.out" "status formatted"
"$qstar" fmt --check "$tmp/fmt/qstar.lua" > "$tmp/fmt-check-after.out" 2> "$tmp/fmt-check-after.err"
contains "$tmp/fmt-check-after.out" "status ok"
"$qstar" --file "$tmp/fmt/qstar.lua" fmt --check > "$tmp/fmt-file-option.out" 2> "$tmp/fmt-file-option.err"
contains "$tmp/fmt-file-option.out" "status ok"

mkdir -p "$tmp/fmt-heavy"
cat > "$tmp/fmt-heavy/qstar.lua" <<'EOF'
local function common_c()
  return {
    include_dirs = {"include"},
    compile_options = {"-Wall"},
  }
end

qstar.executable "app" {
sources={"src/main.c"},
lang={c=common_c()},
}
EOF
"$qstar" fmt --stdout "$tmp/fmt-heavy/qstar.lua" > "$tmp/fmt-heavy.out" 2> "$tmp/fmt-heavy.err"
contains "$tmp/fmt-heavy.out" "local function common_c()"
contains "$tmp/fmt-heavy.out" "return {"
contains "$tmp/fmt-heavy.out" "qstar.executable \"app\" {"
contains "$tmp/fmt-heavy.out" "  sources = {"
contains "$tmp/fmt-heavy.out" "  lang = {"
contains "$tmp/fmt-heavy.out" "    c=common_c(),"

for help_cmd in build test stage dry-run emit-ninja lint fmt list-targets check install last-failure replay docs; do
	"$qstar" "$help_cmd" --help > "$tmp/help-$help_cmd.out" 2> "$tmp/help-$help_cmd.err"
	contains "$tmp/help-$help_cmd.out" "usage: qstar"
done
"$qstar" --help > "$tmp/help-root.out" 2> "$tmp/help-root.err"
contains "$tmp/help-root.out" "usage: qstar"
contains "$tmp/help-root.out" "qstar [options] emit-ninja [label]"
contains "$tmp/help-root.out" "qstar [options] action-log <action-id>"
contains "$tmp/help-root.out" "-G stella|ninja|auto"
"$qstar" help build > "$tmp/help-build-alias.out" 2> "$tmp/help-build-alias.err"
contains "$tmp/help-build-alias.out" "usage: qstar [options] build"
contains "$tmp/help-build-alias.out" "[ 75%] Linking CXX executable app"
contains "$tmp/help-build-alias.out" "--schedule-trace adds scheduler internals"
contains "$tmp/help-build-alias.out" "JSON diagnostics stay uncolored"
not_contains "$tmp/help-build-alias.out" "[5%] Stage"
not_contains "$tmp/help-build-alias.out" "[ 5%] Stage"
"$qstar" help docs > "$tmp/help-docs-alias.out" 2> "$tmp/help-docs-alias.err"
contains "$tmp/help-docs-alias.out" "usage: qstar docs"
"$qstar" docs > "$tmp/docs.out" 2> "$tmp/docs.err"
contains "$tmp/docs.out" "qstar docs v1"
contains "$tmp/docs.out" "wiki/AI_INDEX.md"
"$qstar" docs --ai-index > "$tmp/docs-ai.out" 2> "$tmp/docs-ai.err"
contains "$tmp/docs-ai.out" "wiki/AI_INDEX.md"
"$qstar" docs --path > "$tmp/docs-path.out" 2> "$tmp/docs-path.err"
contains "$tmp/docs-path.out" "/wiki"
"$qstar" docs --show reference/modules.md > "$tmp/docs-show.out" 2> "$tmp/docs-show.err"
contains "$tmp/docs-show.out" "Imports and Modules"
contains "$tmp/docs-show.out" "qstar.import_module"
"$qstar" docs --show reference/qstar-lua.md > "$tmp/docs-show-lua.out" 2> "$tmp/docs-show-lua.err"
contains "$tmp/docs-show-lua.out" "QStar Lua"
contains "$tmp/docs-show-lua.out" "qstar.config"
contains "$tmp/docs-show-lua.out" "qstar.group"
"$qstar" --file "$tmp/qstar.lua" build --help > "$tmp/help-build-with-file.out" 2> "$tmp/help-build-with-file.err"
contains "$tmp/help-build-with-file.out" "usage: qstar [options] build"
"$qstar" --file "$tmp/qstar.lua" check //... > "$tmp/check-all-label.out" 2> "$tmp/check-all-label.err"
contains "$tmp/check-all-label.out" "root <all>"

if "$qstar" --file "$tmp/qstar.lua" lint //:missing > "$tmp/lint-missing-label.out" 2> "$tmp/lint-missing-label.err"; then
	fail "unknown lint label unexpectedly succeeded"
fi
contains "$tmp/lint-missing-label.out" "QSTAR010"
contains "$tmp/lint-missing-label.out" "unknown target label"

lsp_uri="file://$tmp/qstar.lua"
{
	send_lsp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
	send_lsp '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$lsp_uri"'","languageId":"qstar","version":1,"text":"qstar.executable \"app\" {\n  sources = {\"src/main.c\"},\n}\n"}}}'
	send_lsp '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$lsp_uri"'","version":2},"contentChanges":[{"text":"qstar.executable \"app\" {\n  sources = {\"src/main.c\"},\n}\n"}]}}'
	send_lsp '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$lsp_uri"'"},"position":{"line":0,"character":7}}}'
	send_lsp '{"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$lsp_uri"'"},"position":{"line":1,"character":2}}}'
	send_lsp '{"jsonrpc":"2.0","id":4,"method":"shutdown","params":{}}'
	send_lsp '{"jsonrpc":"2.0","method":"exit","params":{}}'
} | "$qstar" lsp --stdio > "$tmp/lsp.out" 2> "$tmp/lsp.err"
contains "$tmp/lsp.out" "\"name\":\"qstar-lsp\""
contains "$tmp/lsp.out" "textDocument/publishDiagnostics"
contains "$tmp/lsp.out" "\"diagnostics\":[]"
contains "$tmp/lsp.out" "Create an executable target."
contains "$tmp/lsp.out" "\"label\":\"qstar.config\""
contains "$tmp/lsp.out" "\"label\":\"qstar.configure_file\""
contains "$tmp/lsp.out" "\"label\":\"qstar.import_module\""
contains "$tmp/lsp.out" "\"label\":\"qstar.merge\""

mkdir -p "$tmp/lsp-missing"
cat > "$tmp/lsp-missing/qstar.lua" <<'EOF'
qstar.subdir("foo")
EOF
lsp_bad_uri="file://$tmp/lsp-missing/qstar.lua"
{
	send_lsp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
	send_lsp '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$lsp_bad_uri"'","languageId":"qstar","version":1,"text":"qstar.subdir(\"foo\")\n"}}}'
	send_lsp '{"jsonrpc":"2.0","id":2,"method":"shutdown","params":{}}'
	send_lsp '{"jsonrpc":"2.0","method":"exit","params":{}}'
} | "$qstar" lsp --stdio > "$tmp/lsp-missing.out" 2> "$tmp/lsp-missing.err"
contains "$tmp/lsp-missing.out" "QSTAR002"
contains "$tmp/lsp-missing.out" "missing fragment"

mkdir -p "$tmp/lsp-nav/lib" "$tmp/lsp-nav/app" \
	"$tmp/lsp-nav/qstar/policies" "$tmp/lsp-nav/qstar/modules/kernel"
cat > "$tmp/lsp-nav/qstar.lua" <<'EOF'
qstar.configure_file "cfg" {
  output = qstar.output("generated/cfg.h"),
  defines = {"HAVE_CFG"},
}

qstar.custom_target "gen" {
  outputs = {qstar.output("generated/gen.c")},
  command = qstar.cli {"tools/gen.sh", qstar.output(0)},
}

qstar.subdir("lib")
qstar.subdir("app")
qstar.import_file("qstar/policies/common.qst")
local kernel = qstar.import_module("qstar/modules/kernel")
EOF
cat > "$tmp/lsp-nav/lib/lib.qst" <<'EOF'
qstar.staticlib "core" {}
EOF
cat > "$tmp/lsp-nav/app/app.qst" <<'EOF'
qstar.executable "app" {
  deps = {"//lib:core"},
}
EOF
cat > "$tmp/lsp-nav/qstar/policies/common.qst" <<'EOF'
local policy = true
EOF
cat > "$tmp/lsp-nav/qstar/modules/kernel/kernel.qsm" <<'EOF'
return {}
EOF
lsp_nav_root_uri="file://$tmp/lsp-nav/qstar.lua"
lsp_nav_lib_uri="file://$tmp/lsp-nav/lib/lib.qst"
lsp_nav_app_uri="file://$tmp/lsp-nav/app/app.qst"
lsp_nav_policy_uri="file://$tmp/lsp-nav/qstar/policies/common.qst"
lsp_nav_module_uri="file://$tmp/lsp-nav/qstar/modules/kernel/kernel.qsm"
{
	send_lsp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
	send_lsp '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$lsp_nav_root_uri"'","languageId":"qstar","version":1,"text":"qstar.configure_file \"cfg\" {\n  output = qstar.output(\"generated/cfg.h\"),\n  defines = {\"HAVE_CFG\"},\n}\n\nqstar.custom_target \"gen\" {\n  outputs = {qstar.output(\"generated/gen.c\")},\n  command = qstar.cli {\"tools/gen.sh\", qstar.output(0)},\n}\n\nqstar.subdir(\"lib\")\nqstar.subdir(\"app\")\nqstar.import_file(\"qstar/policies/common.qst\")\nlocal kernel = qstar.import_module(\"qstar/modules/kernel\")\n"}}}'
	send_lsp '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$lsp_nav_lib_uri"'","languageId":"qstar","version":1,"text":"qstar.staticlib \"core\" {}\n"}}}'
	send_lsp '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$lsp_nav_app_uri"'","languageId":"qstar","version":1,"text":"qstar.executable \"app\" {\n  deps = {\"//lib:core\"},\n}\n"}}}'
	send_lsp '{"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$lsp_nav_app_uri"'"},"position":{"line":1,"character":14}}}'
	send_lsp '{"jsonrpc":"2.0","id":3,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$lsp_nav_app_uri"'"},"position":{"line":1,"character":14},"context":{"includeDeclaration":false}}}'
	send_lsp '{"jsonrpc":"2.0","id":4,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$lsp_nav_lib_uri"'"}}}'
	send_lsp '{"jsonrpc":"2.0","id":5,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$lsp_nav_root_uri"'"}}}'
	send_lsp '{"jsonrpc":"2.0","id":6,"method":"workspace/symbol","params":{"query":"core"}}'
	send_lsp '{"jsonrpc":"2.0","id":7,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$lsp_nav_root_uri"'"},"position":{"line":12,"character":27}}}'
	send_lsp '{"jsonrpc":"2.0","id":8,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$lsp_nav_root_uri"'"},"position":{"line":13,"character":42}}}'
	send_lsp '{"jsonrpc":"2.0","id":9,"method":"shutdown","params":{}}'
	send_lsp '{"jsonrpc":"2.0","method":"exit","params":{}}'
} | "$qstar" lsp --stdio > "$tmp/lsp-nav.out" 2> "$tmp/lsp-nav.err"
contains "$tmp/lsp-nav.out" "\"definitionProvider\":true"
contains "$tmp/lsp-nav.out" "\"referencesProvider\":true"
contains "$tmp/lsp-nav.out" "\"documentSymbolProvider\":true"
contains "$tmp/lsp-nav.out" "\"workspaceSymbolProvider\":true"
contains "$tmp/lsp-nav.out" "\"uri\":\"$lsp_nav_lib_uri\""
contains "$tmp/lsp-nav.out" "\"uri\":\"$lsp_nav_app_uri\""
contains "$tmp/lsp-nav.out" "\"uri\":\"$lsp_nav_policy_uri\""
contains "$tmp/lsp-nav.out" "\"uri\":\"$lsp_nav_module_uri\""
contains "$tmp/lsp-nav.out" "\"name\":\"//lib:core\""
contains "$tmp/lsp-nav.out" "\"name\":\"//:cfg\""
contains "$tmp/lsp-nav.out" "\"name\":\"//:gen\""

step "VSCode extension package metadata" "vscode-package"
vscode_ext="editors/vscode/qstar"
test -f "$vscode_ext/package.json" || fail "missing QStar VSCode package.json"
test -f "$vscode_ext/extension.js" || fail "missing QStar VSCode extension.js"
test -f "$vscode_ext/syntaxes/qstar.tmLanguage.json" || fail "missing QStar grammar"
test -f "$vscode_ext/snippets/qstar.json" || fail "missing QStar snippets"
test -f "$vscode_ext/scripts/package-vsix.sh" || fail "missing QStar VSCode package script"
test -f "$vscode_ext/scripts/check-package.js" || fail "missing QStar VSCode package check"
test -f "$vscode_ext/samples/workspace/qstar.lua" || fail "missing QStar VSCode sample root"
test -f "$vscode_ext/samples/workspace/app/app.qst" || fail "missing QStar VSCode app sample"
test -f "$vscode_ext/samples/workspace/lib/lib.qst" || fail "missing QStar VSCode lib sample"
contains "$vscode_ext/package.json" "\"id\": \"qstar\""
contains "$vscode_ext/package.json" "\"qstar.lua\""
contains "$vscode_ext/package.json" "\".qst\""
contains "$vscode_ext/package.json" "\".qsm\""
contains "$vscode_ext/package.json" "\"version\": \"0.3.0\""
if grep -F '"qstar.workspace"' "$vscode_ext/package.json" >/dev/null 2>&1; then
	fail "qstar.workspace association must stay removed"
fi
contains "$vscode_ext/package.json" "\"package:vsix\""
contains "$vscode_ext/package.json" "\"license\": \"Apache-2.0\""
test -f "$vscode_ext/LICENSE.md" || fail "missing QStar VSCode extension LICENSE.md"
cmp -s LICENSE.md "$vscode_ext/LICENSE.md" || fail "QStar VSCode extension LICENSE.md must match repository LICENSE.md"
contains "$vscode_ext/package.json" "\"url\": \"https://github.com/deeyed/qstar.git\""
contains "$vscode_ext/package.json" "\"directory\": \"editors/vscode/qstar\""
contains "$vscode_ext/.vscodeignore" "*.vsix"
contains "$vscode_ext/.vscodeignore" "dist/"
contains "$vscode_ext/.vscodeignore" "node_modules/"
contains "$vscode_ext/.vscodeignore" "**/build/qstar/**"
contains "$vscode_ext/.vscodeignore" "**/compile_commands.json"
contains "$vscode_ext/package.json" "\"qstar.server.path\""
contains "$vscode_ext/package.json" "\"qstar.trace.server\""
contains "$vscode_ext/package.json" "\"qstarGraph\""
contains "$vscode_ext/package.json" "\"qstar.checkWorkspace\""
contains "$vscode_ext/package.json" "\"qstar.refreshGraph\""
contains "$vscode_ext/package.json" "\"qstar.explainTarget\""
contains "$vscode_ext/package.json" "\"qstar.listTargets\""
contains "$vscode_ext/package.json" "\"qstar.dryRunTarget\""
contains "$vscode_ext/package.json" "\"qstar.buildTarget\""
contains "$vscode_ext/package.json" "\"qstar.testTarget\""
contains "$vscode_ext/package.json" "\"qstar.openActionLog\""
contains "$vscode_ext/package.json" "\"qstar.replayAction\""
contains "$vscode_ext/extension.js" "qstar lsp"
contains "$vscode_ext/extension.js" "registerHoverProvider"
contains "$vscode_ext/extension.js" "registerCompletionItemProvider"
contains "$vscode_ext/extension.js" "registerDefinitionProvider"
contains "$vscode_ext/extension.js" "registerReferenceProvider"
contains "$vscode_ext/extension.js" "registerDocumentSymbolProvider"
contains "$vscode_ext/extension.js" "registerWorkspaceSymbolProvider"
contains "$vscode_ext/extension.js" "registerDocumentFormattingEditProvider"
contains "$vscode_ext/extension.js" "fmt\", \"--stdout"
contains "$vscode_ext/extension.js" "qstar lsp --stdio"
contains "$vscode_ext/extension.js" "registerTreeDataProvider"
contains "$vscode_ext/extension.js" "qstar-targets-v1"
contains "$vscode_ext/extension.js" "last-summary.json"
contains "$vscode_ext/syntaxes/qstar.tmLanguage.json" "entity.name.label.qstar"
contains "$vscode_ext/syntaxes/qstar.tmLanguage.json" "import_module"
contains "$vscode_ext/syntaxes/qstar.tmLanguage.json" "append"
contains "$vscode_ext/snippets/qstar.json" "\"qexe\""
contains "$vscode_ext/snippets/qstar.json" "\"qstaticlib\""
contains "$vscode_ext/snippets/qstar.json" "\"qcustom\""
contains "$vscode_ext/snippets/qstar.json" "\"qtargetconfig\""
contains "$vscode_ext/snippets/qstar.json" "\"qgroup\""
contains "$vscode_ext/snippets/qstar.json" "\"qimportmodule\""
contains "$vscode_ext/snippets/qstar.json" "build/qstar/generated"
contains "$vscode_ext/snippets/qstar.json" "\"qmodule\""
if find "$vscode_ext" -type d -name node_modules | grep . >/dev/null 2>&1; then
	fail "node_modules must not be present under QStar VSCode extension"
fi
if find "$vscode_ext" -path "$vscode_ext/dist" -prune -o -type f -name '*.vsix' -print | grep . >/dev/null 2>&1; then
	fail "VSIX artifacts must not be committed under QStar VSCode extension"
fi
if command -v node >/dev/null 2>&1; then
	node --check "$vscode_ext/extension.js"
	node --check "$vscode_ext/scripts/check-package.js"
	node "$vscode_ext/scripts/check-package.js" "$vscode_ext"
	node -e 'const fs=require("fs"); for (const p of process.argv.slice(1)) JSON.parse(fs.readFileSync(p,"utf8"));' \
		"$vscode_ext/package.json" \
		"$vscode_ext/language-configuration.json" \
		"$vscode_ext/syntaxes/qstar.tmLanguage.json" \
		"$vscode_ext/snippets/qstar.json"
fi

"$qstar" --file "$vscode_ext/samples/workspace/qstar.lua" lint > "$tmp/vscode-sample-lint.out" 2> "$tmp/vscode-sample-lint.err"
contains "$tmp/vscode-sample-lint.out" "qstar lint v1"
contains "$tmp/vscode-sample-lint.out" "status ok"
"$qstar" --file "$vscode_ext/samples/workspace/qstar.lua" list-targets --format json > "$tmp/vscode-sample-targets.out" 2> "$tmp/vscode-sample-targets.err"
contains "$tmp/vscode-sample-targets.out" "\"schema\":\"qstar-targets-v1\""
contains "$tmp/vscode-sample-targets.out" "\"label\":\"//app:app\""
contains "$tmp/vscode-sample-targets.out" "\"label\":\"//lib:core\""
"$qstar" --file "$vscode_ext/samples/workspace/qstar.lua" explain //app:app > "$tmp/vscode-sample-explain.out" 2> "$tmp/vscode-sample-explain.err"
contains "$tmp/vscode-sample-explain.out" "target //app:app"
contains "$tmp/vscode-sample-explain.out" "closure-order [//lib:core, //app:app]"
"$qstar" fmt --check "$vscode_ext/samples/workspace/app/app.qst" > "$tmp/vscode-sample-app-fmt.out" 2> "$tmp/vscode-sample-app-fmt.err"
contains "$tmp/vscode-sample-app-fmt.out" "status ok"
"$qstar" fmt --check "$vscode_ext/samples/workspace/lib/lib.qst" > "$tmp/vscode-sample-lib-fmt.out" 2> "$tmp/vscode-sample-lib-fmt.err"
contains "$tmp/vscode-sample-lib-fmt.out" "status ok"

"$qstar" --file "$tmp/qstar.lua" action-log //:app:compile:0 > "$tmp/action-log.out" 2> "$tmp/action-log.err"
contains "$tmp/action-log.out" "qstar action-log v1"
contains "$tmp/action-log.out" "action //:app:compile:0"
contains "$tmp/action-log.out" "qstar-action-log v2"
contains "$tmp/action-log.out" "argv[0]=cc"
"$qstar" --file "$tmp/qstar.lua" replay //:app:compile:0 > "$tmp/action-replay.out" 2> "$tmp/action-replay.err"
contains "$tmp/action-replay.out" "qstar replay v1"
contains "$tmp/action-replay.out" "action //:app:compile:0"
contains "$tmp/action-replay.out" "cc -c src/main.c"

"$qstar" --file "$tmp/qstar.lua" build //:app --verbose > "$tmp/second.out" 2> "$tmp/second.err"
contains "$tmp/second.out" "status=skip"

"$qstar" --file "$tmp/qstar.lua" why-rebuild //:app > "$tmp/why.out" 2> "$tmp/why.err"
contains "$tmp/why.out" "qstar why-rebuild v1"
contains "$tmp/why.out" "reason=output-check"
contains "$tmp/why.out" "status=skip"
rm -f "$tmp/build/qstar/out/___app/obj0.o"
"$qstar" --file "$tmp/qstar.lua" why-rebuild //:app > "$tmp/why-output.out" 2> "$tmp/why-output.err"
contains "$tmp/why-output.out" "reason=output-missing"

cat > "$tmp/src/main.c" <<'EOF'
int main(void) { return 1 - 1; }
EOF

"$qstar" --file "$tmp/qstar.lua" build //:app --explain-cache --verbose > "$tmp/third.out" 2> "$tmp/third.err"
contains "$tmp/third.out" "cache_miss id=//:app:compile:0"
contains "$tmp/third.out" "reason=input-changed"
contains "$tmp/third.out" "status=run"

"$qstar" --file "$tmp/qstar.lua" log //:app > "$tmp/log.out" 2> "$tmp/log.err"
contains "$tmp/log.out" "qstar log v1"
contains "$tmp/log.out" "log_file build/qstar/logs/___app_compile_0.log"

cat > "$tmp/src/main.c" <<'EOF'
int main(void) { return (1 + ); }
EOF

if "$qstar" --file "$tmp/qstar.lua" --diagnostics json build //:app > "$tmp/fail.out" 2> "$tmp/fail.err"; then
	fail "invalid C build unexpectedly succeeded"
fi
contains "$tmp/fail.err" "\"schema\":\"qstar-diagnostic-v1\""
contains "$tmp/fail.err" "\"field\":\"exit-code\""
contains "$tmp/fail.out" "action_diagnostic_json"
contains "$tmp/build/qstar/state/last-summary.json" "\"status\":\"failure\""
test -f "$tmp/build/qstar/logs/last-failure.replay" || fail "missing failure replay"
if "$qstar" --file "$tmp/qstar.lua" --diagnostics json --color always build //:app --progress off > "$tmp/fail-color.out" 2> "$tmp/fail-color.err"; then
	fail "invalid C color build unexpectedly succeeded"
fi
not_contains "$tmp/fail-color.err" "$esc"
contains "$tmp/fail-color.out" "${esc}[1;31mstatus fail${esc}[0m"
grep '^action_diagnostic_json ' "$tmp/fail-color.out" > "$tmp/fail-color-action-json.out" || fail "missing action diagnostic json in color failure"
not_contains "$tmp/fail-color-action-json.out" "$esc"

"$qstar" --file "$tmp/qstar.lua" last-failure > "$tmp/replay.out" 2> "$tmp/replay.err"
contains "$tmp/replay.out" "qstar last-failure v1"
contains "$tmp/replay.out" "cc -c src/main.c"

"$qstar" --file "$tmp/qstar.lua" clean --target //:app > "$tmp/clean-target.out" 2> "$tmp/clean-target.err"
contains "$tmp/clean-target.out" "qstar clean v1"
test ! -d "$tmp/build/qstar/out/___app" || fail "target clean left target output"

"$qstar" --file "$tmp/qstar.lua" clean > "$tmp/clean.out" 2> "$tmp/clean.err"
contains "$tmp/clean.out" "clean_all build/qstar compile_commands=build"
test ! -d "$tmp/build/qstar" || fail "clean left build/qstar"
test ! -f "$tmp/build/qstar/compile_commands.json" || fail "clean left compile_commands.json"

mkdir -p "$tmp/lint-canonical/foo"
cat > "$tmp/lint-canonical/qstar.lua" <<'EOF'
qstar.subdir("foo")
EOF
cat > "$tmp/lint-canonical/foo/foo.qst" <<'EOF'
qstar.staticlib "core" {}
EOF
"$qstar" --file "$tmp/lint-canonical/qstar.lua" lint //... > "$tmp/lint-canonical.out" 2> "$tmp/lint-canonical.err"
contains "$tmp/lint-canonical.out" "status ok"

mkdir -p "$tmp/lint-removed-qs/foo"
cat > "$tmp/lint-removed-qs/qstar.lua" <<'EOF'
qstar.executable "app" {}
EOF
cat > "$tmp/lint-removed-qs/foo/foo.qs" <<'EOF'
qstar.staticlib "core" {}
EOF
if "$qstar" --file "$tmp/lint-removed-qs/qstar.lua" lint --format json > "$tmp/lint-removed-qs.out" 2> "$tmp/lint-removed-qs.err"; then
	fail "removed .qs fragment lint unexpectedly succeeded"
fi
contains "$tmp/lint-removed-qs.out" "\"code\":\"QSTAR003\""
contains "$tmp/lint-removed-qs.out" ".qs fragments were removed"

mkdir -p "$tmp/lint-removed-workspace"
cat > "$tmp/lint-removed-workspace/qstar.lua" <<'EOF'
qstar.executable "app" {}
EOF
touch "$tmp/lint-removed-workspace/qstar.workspace"
if "$qstar" --file "$tmp/lint-removed-workspace/qstar.lua" lint --format json > "$tmp/lint-removed-workspace.out" 2> "$tmp/lint-removed-workspace.err"; then
	fail "removed qstar.workspace lint unexpectedly succeeded"
fi
contains "$tmp/lint-removed-workspace.out" "\"code\":\"QSTAR004\""
contains "$tmp/lint-removed-workspace.out" "qstar.workspace was removed"

mkdir -p "$tmp/lint-missing"
cat > "$tmp/lint-missing/qstar.lua" <<'EOF'
qstar.subdir("foo")
EOF
if "$qstar" --file "$tmp/lint-missing/qstar.lua" lint > "$tmp/lint-missing.out" 2> "$tmp/lint-missing.err"; then
	fail "missing subdir fragment unexpectedly succeeded"
fi
contains "$tmp/lint-missing.out" "QSTAR002"
contains "$tmp/lint-missing.out" "missing fragment"

badroot_tmp="${tmp}.badroot"
rm -rf "$badroot_tmp"
mkdir -p "$badroot_tmp"
cat > "$badroot_tmp/build.lua" <<'EOF'
qstar.executable "app" {}
EOF
if "$qstar" --file "$badroot_tmp/build.lua" lint > "$tmp/lint-badroot.out" 2> "$tmp/lint-badroot.err"; then
	fail "bad root file naming unexpectedly succeeded"
fi
contains "$tmp/lint-badroot.out" "QSTAR001"
contains "$tmp/lint-badroot.out" "could not find qstar.lua"
rm -rf "$badroot_tmp"

mkdir -p "$tmp/lint-outside"
cat > "$tmp/lint-outside/qstar.lua" <<'EOF'
qstar.executable "bad" {
  sources = {"../escape.c"},
}
EOF
if "$qstar" --file "$tmp/lint-outside/qstar.lua" lint > "$tmp/lint-outside.out" 2> "$tmp/lint-outside.err"; then
	fail "package-escaping source lint unexpectedly succeeded"
fi
contains "$tmp/lint-outside.out" "QSTAR020"
contains "$tmp/lint-outside.out" "must be package-relative"

mkdir -p "$tmp/lint-duplicate"
cat > "$tmp/lint-duplicate/qstar.lua" <<'EOF'
qstar.executable "dup" {}
qstar.staticlib "dup" {}
EOF
if "$qstar" --file "$tmp/lint-duplicate/qstar.lua" lint > "$tmp/lint-duplicate.out" 2> "$tmp/lint-duplicate.err"; then
	fail "duplicate target lint unexpectedly succeeded"
fi
contains "$tmp/lint-duplicate.out" "QSTAR011"
contains "$tmp/lint-duplicate.out" "duplicate target label"

mkdir -p "$tmp/lint-badlabel"
cat > "$tmp/lint-badlabel/qstar.lua" <<'EOF'
qstar.executable "bad label" {}
EOF
if "$qstar" --file "$tmp/lint-badlabel/qstar.lua" lint > "$tmp/lint-badlabel.out" 2> "$tmp/lint-badlabel.err"; then
	fail "bad label lint unexpectedly succeeded"
fi
contains "$tmp/lint-badlabel.out" "QSTAR010"
contains "$tmp/lint-badlabel.out" "invalid target name"

mkdir -p "$tmp/old-api"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.exe "app" {}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-api.out" 2> "$tmp/old-api.err"; then
	fail "removed qstar.exe unexpectedly succeeded"
fi
contains "$tmp/old-api.err" "qstar.exe removed; use qstar.executable"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.genrule "g" {}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-genrule.out" 2> "$tmp/old-genrule.err"; then
	fail "removed qstar.genrule unexpectedly succeeded"
fi
contains "$tmp/old-genrule.err" "qstar.genrule removed; use qstar.custom_target"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.config_header "cfg" {}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-config.out" 2> "$tmp/old-config.err"; then
	fail "removed qstar.config_header unexpectedly succeeded"
fi
contains "$tmp/old-config.err" "qstar.config_header removed; use qstar.configure_file"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.executable "app" {
  include_dirs = {"include"},
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-include.out" 2> "$tmp/old-include.err"; then
	fail "top-level include_dirs unexpectedly succeeded"
fi
contains "$tmp/old-include.err" "top-level include_dirs is not allowed; move it under lang.c.include_dirs"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.executable "app" {
  cxx_standard = "c++20",
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-cxx-standard.out" 2> "$tmp/old-cxx-standard.err"; then
	fail "top-level cxx_standard unexpectedly succeeded"
fi
contains "$tmp/old-cxx-standard.err" "top-level cxx_standard is not allowed; move it to lang.cxx.standard"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  public_headers = {"include/core.h"},
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-public-headers.out" 2> "$tmp/old-public-headers.err"; then
	fail "top-level public_headers unexpectedly succeeded"
fi
contains "$tmp/old-public-headers.err" "top-level public_headers is not allowed; move it under lang.c.public_headers"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  private_headers = {"src/core_private.h"},
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-private-headers.out" 2> "$tmp/old-private-headers.err"; then
	fail "top-level private_headers unexpectedly succeeded"
fi
contains "$tmp/old-private-headers.err" "top-level private_headers is not allowed; move it under lang.c.private_headers"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  modules = qstar.modules { root = "src" },
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-modules.out" 2> "$tmp/old-modules.err"; then
	fail "top-level modules unexpectedly succeeded"
fi
contains "$tmp/old-modules.err" "top-level modules is not allowed; move it under lang.cale.modules or lang.cxx.modules"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  hcl_include_dirs = {"include"},
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-top-hcl-include.out" 2> "$tmp/old-top-hcl-include.err"; then
	fail "top-level hcl_include_dirs unexpectedly succeeded"
fi
contains "$tmp/old-top-hcl-include.err" "hcl_include_dirs is removed; use lang.cale.public_include_dirs or lang.cale.private_include_dirs"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  lang = {
    cale = {
      hcl_include_dirs = {"include"},
    },
  },
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-hcl-include.out" 2> "$tmp/old-hcl-include.err"; then
	fail "lang.cale.hcl_include_dirs unexpectedly succeeded"
fi
contains "$tmp/old-hcl-include.err" "unknown field lang.cale.hcl_include_dirs"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  lang = {
    cxx = {
      modules = { enabled = true },
    },
  },
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/cxx-modules-enabled.out" 2> "$tmp/cxx-modules-enabled.err"; then
	fail "enabled C++ modules unexpectedly succeeded"
fi
contains "$tmp/cxx-modules-enabled.err" "C++ modules are not supported; set lang.cxx.modules.enabled = false"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.custom_target "g" {
  tool = "tools/gen.sh",
  outputs = {qstar.output("generated/g.c")},
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-custom-tool.out" 2> "$tmp/old-custom-tool.err"; then
	fail "custom_target tool syntax unexpectedly succeeded"
fi
contains "$tmp/old-custom-tool.err" "command = qstar.cli"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    rust = {
      include_dirs = {"include"},
    },
  },
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" lint --format json > "$tmp/lang-rust.out" 2> "$tmp/lang-rust.err"; then
	fail "unknown lang namespace unexpectedly succeeded"
fi
contains "$tmp/lang-rust.out" "unknown language namespace lang.rust"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    c = {
      unknown_option = true,
    },
  },
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" lint --format json > "$tmp/lang-c-field.out" 2> "$tmp/lang-c-field.err"; then
	fail "unknown lang.c field unexpectedly succeeded"
fi
contains "$tmp/lang-c-field.out" "unknown field lang.c.unknown_option"

mkdir -p "$tmp/lint-header-source/include" "$tmp/lint-header-source/src"
cat > "$tmp/lint-header-source/include/app.h" <<'EOF'
#define APP_VALUE 1
EOF
cat > "$tmp/lint-header-source/src/main.c" <<'EOF'
#include "app.h"
int main(void) { return APP_VALUE - 1; }
EOF
cat > "$tmp/lint-header-source/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c", "include/app.h"},
  lang = {
    c = {
      include_dirs = {"include"},
    },
  },
}
EOF
"$qstar" --file "$tmp/lint-header-source/qstar.lua" lint --format json > "$tmp/lint-header-source.out" 2> "$tmp/lint-header-source.err"
contains "$tmp/lint-header-source.out" "\"code\":\"QSTAR040\""
contains "$tmp/lint-header-source.out" "use lang.*.public_headers/private_headers"
contains "$tmp/lint-header-source.out" "\"status\":\"warning\""
"$qstar" --file "$tmp/lint-header-source/qstar.lua" --color always lint > "$tmp/lint-header-source-color.out" 2> "$tmp/lint-header-source-color.err"
contains "$tmp/lint-header-source-color.out" "${esc}[1;33mwarning${esc}[0m"
contains "$tmp/lint-header-source-color.out" "status ${esc}[1;33mwarning${esc}[0m"

mkdir -p "$tmp/lint-public-generated"
cat > "$tmp/lint-public-generated/qstar.lua" <<'EOF'
qstar.configure_file "cfg" {
  output = qstar.output("generated/public_config.h"),
  defines = {"HAVE_CFG"},
}

qstar.staticlib "core" {
  lang = {
    c = {
      public_headers = {qstar.output("generated/public_config.h")},
    },
  },
}
EOF
"$qstar" --file "$tmp/lint-public-generated/qstar.lua" lint --format json > "$tmp/lint-public-generated.out" 2> "$tmp/lint-public-generated.err"
contains "$tmp/lint-public-generated.out" "\"code\":\"QSTAR041\""
contains "$tmp/lint-public-generated.out" "outside include/"

mkdir -p "$tmp/lint-private-dep/include" "$tmp/lint-private-dep/src"
cat > "$tmp/lint-private-dep/include/lib.h" <<'EOF'
int lib_value(void);
EOF
cat > "$tmp/lint-private-dep/include/wrapper.h" <<'EOF'
int wrapper_value(void);
EOF
cat > "$tmp/lint-private-dep/src/lib.c" <<'EOF'
int lib_value(void) { return 1; }
EOF
cat > "$tmp/lint-private-dep/src/wrapper.c" <<'EOF'
int wrapper_value(void) { return 2; }
EOF
cat > "$tmp/lint-private-dep/qstar.lua" <<'EOF'
qstar.staticlib "lib" {
  sources = {"src/lib.c"},
  lang = {
    c = {
      public_headers = {"include/lib.h"},
      public_include_dirs = {"include"},
    },
  },
}

qstar.staticlib "wrapper" {
  sources = {"src/wrapper.c"},
  lang = {
    c = {
      public_headers = {"include/wrapper.h"},
    },
  },
  private_deps = {"//:lib"},
}
EOF
"$qstar" --file "$tmp/lint-private-dep/qstar.lua" lint > "$tmp/lint-private-dep.out" 2> "$tmp/lint-private-dep.err"
contains "$tmp/lint-private-dep.out" "QSTAR042"
contains "$tmp/lint-private-dep.out" "private dependency '//:lib'"

mkdir -p "$tmp/lint-duplicate-source/src"
cat > "$tmp/lint-duplicate-source/src/shared.c" <<'EOF'
int shared(void) { return 0; }
EOF
cat > "$tmp/lint-duplicate-source/qstar.lua" <<'EOF'
qstar.staticlib "one" {
  sources = {"src/shared.c"},
}

qstar.staticlib "two" {
  sources = {"src/shared.c"},
}
EOF
"$qstar" --file "$tmp/lint-duplicate-source/qstar.lua" lint > "$tmp/lint-duplicate-source.out" 2> "$tmp/lint-duplicate-source.err"
contains "$tmp/lint-duplicate-source.out" "QSTAR043"
contains "$tmp/lint-duplicate-source.out" "used by both '//:one' and '//:two'"

mkdir -p "$tmp/lint-target-family/src"
cat > "$tmp/lint-target-family/src/shared.c" <<'EOF'
int shared(void) { return 0; }
EOF
cat > "$tmp/lint-target-family/qstar.lua" <<'EOF'
qstar.target_family "boot" {
  variants = {"x86_64", "aarch64"},
  allow_shared_sources = true,
}

qstar.staticlib "boot_x86_64" {
  sources = {"src/shared.c"},
}

qstar.staticlib "boot_aarch64" {
  sources = {"src/shared.c"},
}
EOF
"$qstar" --file "$tmp/lint-target-family/qstar.lua" lint > "$tmp/lint-target-family.out" 2> "$tmp/lint-target-family.err"
contains "$tmp/lint-target-family.out" "status ok"
if grep -F -q -- "QSTAR043" "$tmp/lint-target-family.out"; then
	fail "target_family allow_shared_sources did not suppress duplicate source lint"
fi
"$qstar" --file "$tmp/lint-target-family/qstar.lua" list-targets --format json > "$tmp/lint-target-family-json.out" 2> "$tmp/lint-target-family-json.err"
contains "$tmp/lint-target-family-json.out" "\"target_family_count\":1"
contains "$tmp/lint-target-family-json.out" "\"allow_shared_sources\":true"
cat > "$tmp/lint-target-family/qstar.lua" <<'EOF'
qstar.target_family "manual" {
  variants = {"one", "two"},
  targets = {"//:one", "//:missing"},
  allow_shared_sources = true,
}

qstar.staticlib "one" {
  sources = {"src/shared.c"},
}
EOF
if "$qstar" --file "$tmp/lint-target-family/qstar.lua" check > "$tmp/lint-target-family-missing.out" 2> "$tmp/lint-target-family-missing.err"; then
	fail "unknown target_family target unexpectedly succeeded"
fi
contains "$tmp/lint-target-family-missing.err" "target_family 'manual' references unknown target '//:missing'"

mkdir -p "$tmp/lint-cxx-info/src"
cat > "$tmp/lint-cxx-info/src/main.cpp" <<'EOF'
int main() { return 0; }
EOF
cat > "$tmp/lint-cxx-info/qstar.lua" <<'EOF'
qstar.executable "cpp" {
  sources = {"src/main.cpp"},
}
EOF
"$qstar" --file "$tmp/lint-cxx-info/qstar.lua" lint --format json > "$tmp/lint-cxx-info.out" 2> "$tmp/lint-cxx-info.err"
contains "$tmp/lint-cxx-info.out" "\"code\":\"QSTAR044\""
contains "$tmp/lint-cxx-info.out" "\"severity\":\"info\""
contains "$tmp/lint-cxx-info.out" "\"status\":\"ok\""

mkdir -p "$tmp/lint-cale-toolchain/src"
cat > "$tmp/lint-cale-toolchain/src/plugin.cale" <<'EOF'
fn plugin() -> int { return 0; }
EOF
cat > "$tmp/lint-cale-toolchain/qstar.lua" <<'EOF'
qstar.staticlib "plugin" {
  sources = {"src/plugin.cale"},
}
EOF
"$qstar" --file "$tmp/lint-cale-toolchain/qstar.lua" lint > "$tmp/lint-cale-toolchain.out" 2> "$tmp/lint-cale-toolchain.err"
contains "$tmp/lint-cale-toolchain.out" "QSTAR045"
contains "$tmp/lint-cale-toolchain.out" "toolchain=\"cale\""

mkdir -p "$tmp/lint-visibility"
cat > "$tmp/lint-visibility/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  visibility = {"//app:*"},
}
EOF
if "$qstar" --file "$tmp/lint-visibility/qstar.lua" lint > "$tmp/lint-visibility.out" 2> "$tmp/lint-visibility.err"; then
	fail "invalid visibility lint unexpectedly succeeded"
fi
contains "$tmp/lint-visibility.out" "QSTAR050"
contains "$tmp/lint-visibility.out" "invalid visibility pattern"

mkdir -p "$tmp/lint-output-collision"
cat > "$tmp/lint-output-collision/qstar.lua" <<'EOF'
qstar.custom_target "one" {
  outputs = {qstar.output("generated/same.c")},
  command = qstar.cli {"tools/gen.sh", qstar.output(0)},
}

qstar.custom_target "two" {
  outputs = {qstar.output("generated/same.c")},
  command = qstar.cli {"tools/gen.sh", qstar.output(0)},
}
EOF
if "$qstar" --file "$tmp/lint-output-collision/qstar.lua" lint --format json > "$tmp/lint-output-collision.out" 2> "$tmp/lint-output-collision.err"; then
	fail "generated collision lint unexpectedly succeeded"
fi
contains "$tmp/lint-output-collision.out" "\"code\":\"QSTAR060\""
contains "$tmp/lint-output-collision.out" "multiple producers"

mkdir -p "$tmp/lint-orphan/foo"
cat > "$tmp/lint-orphan/qstar.lua" <<'EOF'
qstar.executable "app" {}
EOF
cat > "$tmp/lint-orphan/foo/foo.qst" <<'EOF'
qstar.staticlib "core" {}
EOF
"$qstar" --file "$tmp/lint-orphan/qstar.lua" lint --format json > "$tmp/lint-orphan.out" 2> "$tmp/lint-orphan.err"
contains "$tmp/lint-orphan.out" "\"code\":\"QSTAR071\""
contains "$tmp/lint-orphan.out" "not reached by qstar.subdir()"

mkdir -p "$tmp/tools"
cat > "$tmp/tools/gen-value.sh" <<'EOF'
#!/bin/sh
set -eu
out=$1
mkdir -p "$(dirname "$out")"
cat > "$out" <<'SRC'
int generated_value(void) { return 41; }
SRC
EOF
chmod +x "$tmp/tools/gen-value.sh"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"APP_VALUE=41", "APP_FEATURE"},
}

qstar.custom_target "make_value" {
  outputs = {qstar.output("generated/value.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}

qstar.executable "genapp" {
  sources = {"src/main.c", qstar.output("generated/value.c")},
  lang = {
    c = {
      private_headers = {qstar.output("generated/config.h")},
      include_dirs = {"generated"},
    },
  },
}

qstar.run_target "smoke" {
  deps = {"//:genapp"},
  command = qstar.cli {qstar.target_file("//:genapp")},
  timeout = 5,
}

qstar.run_target "marker" {
  command = qstar.cli {"printf", "QSTAR-MARKER\n"},
  timeout = 5,
  marker = "QSTAR-MARKER",
}
EOF

cat > "$tmp/src/main.c" <<'EOF'
#include "config.h"
int generated_value(void);
int main(void) { return generated_value() - APP_VALUE; }
EOF

"$qstar" --file "$tmp/qstar.lua" build //:genapp --verbose > "$tmp/generated-first.out" 2> "$tmp/generated-first.err"
contains "$tmp/generated-first.out" "build_action id=//:cfg:generate:0 status=run"
contains "$tmp/generated-first.out" "build_action id=//:make_value:generate:0 status=run"
contains "$tmp/generated-first.out" "status ok"
test -f "$tmp/generated/config.h" || fail "missing generated config header"
test -f "$tmp/generated/value.c" || fail "missing generated source"
contains "$tmp/generated/config.h" "#define APP_VALUE 41"
"$qstar" --file "$tmp/qstar.lua" build //:smoke > "$tmp/run-target-smoke.out" 2> "$tmp/run-target-smoke.err"
contains "$tmp/run-target-smoke.out" "run_target label=//:smoke command=argv"
contains "$tmp/run-target-smoke.out" "status ok"
"$qstar" --file "$tmp/qstar.lua" -B build/async-run build //:smoke --schedule-trace --progress off --color never > "$tmp/run-target-async.out" 2> "$tmp/run-target-async.err"
contains "$tmp/run-target-async.out" "schedule_action id=//:smoke:run:0 kind=run slot="
contains "$tmp/run-target-async.out" "parallel_event target=//:smoke event=start id=//:smoke:run:0"
"$qstar" --file "$tmp/qstar.lua" build //:marker > "$tmp/run-target-marker.out" 2> "$tmp/run-target-marker.err"
contains "$tmp/run-target-marker.out" "run_marker label=//:marker status=matched"
contains "$tmp/run-target-marker.out" "status ok"

mkdir -p "$tmp/qemu/tools"
cat > "$tmp/qemu/tools/fake-qemu.sh" <<'EOF'
#!/bin/sh
set -eu
mode=$1
serial=$2
case "$mode" in
	ok)
		printf "booting\n"
		printf "SERIAL-READY\n" > "$serial"
		;;
	missing)
		printf "booting without marker\n"
		printf "NO-MARKER\n" > "$serial"
		;;
	exit)
		printf "fatal boot error\n" >&2
		exit 7
		;;
	timeout)
		sleep 5
		;;
esac
EOF
chmod +x "$tmp/qemu/tools/fake-qemu.sh"
cat > "$tmp/qemu/qstar.lua" <<'EOF'
qstar.run_target "qemu_ok" {
  command = qstar.cli {"tools/fake-qemu.sh", "ok", "serial.log"},
  timeout = 2,
  marker = "SERIAL-READY",
  marker_log = "serial.log",
}

qstar.run_target "qemu_marker_missing" {
  command = qstar.cli {"tools/fake-qemu.sh", "missing", "serial.log"},
  timeout = 2,
  marker = "SERIAL-READY",
  marker_log = "serial.log",
}

qstar.run_target "qemu_exit" {
  command = qstar.cli {"tools/fake-qemu.sh", "exit", "serial.log"},
  timeout = 2,
  marker = "SERIAL-READY",
  marker_log = "serial.log",
}

qstar.run_target "qemu_timeout" {
  command = qstar.cli {"tools/fake-qemu.sh", "timeout", "serial.log"},
  timeout = 1,
  marker = "SERIAL-READY",
  marker_log = "serial.log",
}
EOF
"$qstar" --file "$tmp/qemu/qstar.lua" build //:qemu_ok > "$tmp/qemu-ok.out" 2> "$tmp/qemu-ok.err"
contains "$tmp/qemu-ok.out" "run_target label=//:qemu_ok command=argv timeout_sec=2 marker=SERIAL-READY marker_log=serial.log"
contains "$tmp/qemu-ok.out" "run_marker label=//:qemu_ok status=matched marker=SERIAL-READY source=marker_log path=serial.log"
contains "$tmp/qemu-ok.out" "status ok"
if "$qstar" --file "$tmp/qemu/qstar.lua" build //:qemu_marker_missing > "$tmp/qemu-missing.out" 2> "$tmp/qemu-missing.err"; then
	fail "qemu marker missing unexpectedly succeeded"
fi
contains "$tmp/qemu-missing.out" "run_target_result label=//:qemu_marker_missing status=marker-missing marker=SERIAL-READY"
contains "$tmp/qemu-missing.out" "marker_log=serial.log"
contains "$tmp/qemu-missing.err" "marker 'SERIAL-READY' was not found"
contains "$tmp/qemu/build/qstar/logs/last-failure.replay" "failure_kind=marker-missing"
contains "$tmp/qemu/build/qstar/logs/last-failure.replay" "marker_log=serial.log"
"$qstar" --file "$tmp/qemu/qstar.lua" last-failure > "$tmp/qemu-last-failure.out" 2> "$tmp/qemu-last-failure.err"
contains "$tmp/qemu-last-failure.out" "qstar last-failure v1"
contains "$tmp/qemu-last-failure.out" "failure_kind=marker-missing"
contains "$tmp/qemu-last-failure.out" "tools/fake-qemu.sh missing serial.log"
"$qstar" --file "$tmp/qemu/qstar.lua" replay //:qemu_marker_missing:run:0 > "$tmp/qemu-replay.out" 2> "$tmp/qemu-replay.err"
contains "$tmp/qemu-replay.out" "qstar replay v1"
contains "$tmp/qemu-replay.out" "tools/fake-qemu.sh missing serial.log"
if "$qstar" --file "$tmp/qemu/qstar.lua" build //:qemu_exit > "$tmp/qemu-exit.out" 2> "$tmp/qemu-exit.err"; then
	fail "qemu exit failure unexpectedly succeeded"
fi
contains "$tmp/qemu-exit.out" "run_target_result label=//:qemu_exit status=exit-code exit=7"
contains "$tmp/qemu-exit.err" "failed with exit code 7"
contains "$tmp/qemu/build/qstar/logs/last-failure.replay" "failure_kind=exit-code"
if "$qstar" --file "$tmp/qemu/qstar.lua" build //:qemu_timeout > "$tmp/qemu-timeout.out" 2> "$tmp/qemu-timeout.err"; then
	fail "qemu timeout unexpectedly succeeded"
fi
contains "$tmp/qemu-timeout.out" "run_target_result label=//:qemu_timeout status=timeout timeout_sec=1"
contains "$tmp/qemu-timeout.out" "action_diagnostic_json"
contains "$tmp/qemu-timeout.out" "\"failure_kind\":\"qemu-timeout\""
contains "$tmp/qemu-timeout.err" "timed out after 1 seconds"
contains "$tmp/qemu/build/qstar/logs/last-failure.replay" "failure_kind=qemu-timeout"

mkdir -p "$tmp/qstar/status"
cat > "$tmp/qstar/status/status.qsm" <<'EOF'
local M = {}

function M.step(verb, path)
  return qstar.status(verb .. " " .. path)
end

return M
EOF

cat > "$tmp/qstar.lua" <<'EOF'
local status = qstar.import_module("qstar/status")

qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"APP_VALUE=42", "APP_FEATURE"},
  description = qstar.status("Configuring generated config.h"),
}

qstar.custom_target "make_value" {
  outputs = {qstar.output("generated/value.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
  description = status.step("Generating", "generated/value.c"),
}

qstar.executable "genapp" {
  sources = {"src/main.c", qstar.output("generated/value.c")},
  lang = {
    c = {
      private_headers = {qstar.output("generated/config.h")},
      include_dirs = {"generated"},
    },
  },
}
EOF

"$qstar" --file "$tmp/qstar.lua" dry-run //:genapp > "$tmp/generated-description-dry.out" 2> "$tmp/generated-description-dry.err"
contains "$tmp/generated-description-dry.out" "action_description id=//:cfg:generate:0 text=\"Configuring generated config.h\""
contains "$tmp/generated-description-dry.out" "action_description id=//:make_value:generate:0 text=\"Generating generated/value.c\""
"$qstar" --file "$tmp/qstar.lua" build //:genapp --explain-cache > "$tmp/generated-second.out" 2> "$tmp/generated-second.err"
contains "$tmp/generated-second.out" "cache_miss id=//:cfg:generate:0"
contains "$tmp/generated-second.out" "cache_miss id=//:genapp:compile:0"
contains "$tmp/generated/config.h" "#define APP_VALUE 42"

mkdir -p "$tmp/bad-status-raw" "$tmp/bad-status-empty" "$tmp/bad-status-newline" "$tmp/bad-status-long"
cat > "$tmp/bad-status-raw/qstar.lua" <<'EOF'
qstar.custom_target "bad" {
  description = "raw string",
  outputs = {qstar.output("generated/bad.txt")},
  command = qstar.cli {"printf", "bad"},
}
EOF
if "$qstar" --file "$tmp/bad-status-raw/qstar.lua" check > "$tmp/bad-status-raw.out" 2> "$tmp/bad-status-raw.err"; then
  fail "raw description unexpectedly succeeded"
fi
contains "$tmp/bad-status-raw.err" "field 'description' must be qstar.status"

cat > "$tmp/bad-status-empty/qstar.lua" <<'EOF'
qstar.run_target "bad" {
  command = qstar.cli {"true"},
  description = qstar.status(""),
}
EOF
if "$qstar" --file "$tmp/bad-status-empty/qstar.lua" check > "$tmp/bad-status-empty.out" 2> "$tmp/bad-status-empty.err"; then
  fail "empty qstar.status unexpectedly succeeded"
fi
contains "$tmp/bad-status-empty.err" "qstar.status description must not be empty"

cat > "$tmp/bad-status-newline/qstar.lua" <<'EOF'
qstar.run_target "bad" {
  command = qstar.cli {"true"},
  description = qstar.status("bad\nline"),
}
EOF
if "$qstar" --file "$tmp/bad-status-newline/qstar.lua" check > "$tmp/bad-status-newline.out" 2> "$tmp/bad-status-newline.err"; then
  fail "newline qstar.status unexpectedly succeeded"
fi
contains "$tmp/bad-status-newline.err" "qstar.status description must be one line"

cat > "$tmp/bad-status-long/qstar.lua" <<'EOF'
qstar.run_target "bad" {
  command = qstar.cli {"true"},
  description = qstar.status(string.rep("a", 241)),
}
EOF
if "$qstar" --file "$tmp/bad-status-long/qstar.lua" check > "$tmp/bad-status-long.out" 2> "$tmp/bad-status-long.err"; then
  fail "long qstar.status unexpectedly succeeded"
fi
contains "$tmp/bad-status-long.err" "qstar.status description must be <= 240 bytes"

mkdir -p "$tmp/generated-root/src" "$tmp/generated-root/tools"
cp "$tmp/tools/gen-value.sh" "$tmp/generated-root/tools/gen-value.sh"
cat > "$tmp/generated-root/src/main.c" <<'EOF'
#include "config.h"
int generated_value(void);
int main(void) { return generated_value() - APP_VALUE; }
EOF
cat > "$tmp/generated-root/qstar.lua" <<'EOF'
qstar.project {
  name = "generated-root",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.configure_file "cfg" {
  output = qstar.output("build/qstar/generated/config.h"),
  defines = {"APP_VALUE=41"},
}

qstar.custom_target "make_value" {
  outputs = {qstar.output("build/qstar/generated/value.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}

qstar.executable "app" {
  sources = {"src/main.c", qstar.output("build/qstar/generated/value.c")},
  lang = {
    c = {
      private_headers = {qstar.output("build/qstar/generated/config.h")},
      include_dirs = {"build/qstar/generated"},
    },
  },
}
EOF
"$qstar" --file "$tmp/generated-root/qstar.lua" build //:app > "$tmp/generated-root-build.out" 2> "$tmp/generated-root-build.err"
contains "$tmp/generated-root-build.out" "status ok"
test -f "$tmp/generated-root/build/qstar/generated/config.h" || fail "configured generated_dir missing config header"
test -f "$tmp/generated-root/build/qstar/generated/value.c" || fail "configured generated_dir missing source"
test ! -e "$tmp/generated-root/generated" || fail "configured generated_dir polluted package root generated/"
"$tmp/generated-root/build/qstar/out/___app/app"
contains "$tmp/generated-root/build/qstar/compile_commands.json" "build/qstar/generated/value.c"
QSTAR_DEBUG_STATE_DUMPS=1 "$qstar" --file "$tmp/generated-root/qstar.lua" build //:app --progress off > "$tmp/generated-root-debug-state.out" 2> "$tmp/generated-root-debug-state.err"
contains "$tmp/generated-root/build/qstar/state/graph.json" "\"generated_dir\":\"build/qstar/generated\""
"$qstar" --file "$tmp/generated-root/qstar.lua" list-targets --format json > "$tmp/generated-root-list.json" 2> "$tmp/generated-root-list.err"
contains "$tmp/generated-root-list.json" "\"generated_dir\":\"build/qstar/generated\""
"$qstar" --file "$tmp/generated-root/qstar.lua" --dump-graph > "$tmp/generated-root-graph.out" 2> "$tmp/generated-root-graph.err"
contains "$tmp/generated-root-graph.out" "generated_dir=build/qstar/generated"

mkdir -p "$tmp/generated-root-bad-output"
cp -R "$tmp/generated-root/tools" "$tmp/generated-root-bad-output/tools"
cat > "$tmp/generated-root-bad-output/qstar.lua" <<'EOF'
qstar.project {
  root = ".",
  generated_dir = "build/qstar/generated",
}

qstar.custom_target "bad" {
  outputs = {qstar.output("generated/bad.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}
EOF
if "$qstar" --file "$tmp/generated-root-bad-output/qstar.lua" check //:bad > "$tmp/generated-root-bad-output.out" 2> "$tmp/generated-root-bad-output.err"; then
	fail "generated_dir bad output unexpectedly succeeded"
fi
contains "$tmp/generated-root-bad-output.err" "must be under generated_dir 'build/qstar/generated'"

mkdir -p "$tmp/generated-root-bad-source"
cat > "$tmp/generated-root-bad-source/qstar.lua" <<'EOF'
qstar.project {
  root = ".",
  generated_dir = "build/qstar/generated",
}

qstar.executable "bad" {
  sources = {qstar.output("build/qstar/generated/missing.c")},
}
EOF
if "$qstar" --file "$tmp/generated-root-bad-source/qstar.lua" check //:bad > "$tmp/generated-root-bad-source.out" 2> "$tmp/generated-root-bad-source.err"; then
	fail "generated_dir orphan source unexpectedly succeeded"
fi
contains "$tmp/generated-root-bad-source.err" "has no generating action"

mkdir -p "$tmp/generated-root-bad-project"
cat > "$tmp/generated-root-bad-project/qstar.lua" <<'EOF'
qstar.project {
  root = ".",
  generated_dir = "build/qstar/generated/",
}
EOF
if "$qstar" --file "$tmp/generated-root-bad-project/qstar.lua" check //... > "$tmp/generated-root-bad-project.out" 2> "$tmp/generated-root-bad-project.err"; then
	fail "generated_dir trailing slash unexpectedly succeeded"
fi
contains "$tmp/generated-root-bad-project.err" "generated_dir must name a directory without a trailing slash"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.custom_target "one" {
  outputs = {qstar.output("generated/collision.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}

qstar.custom_target "two" {
  outputs = {qstar.output("generated/collision.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}
EOF

if "$qstar" --file "$tmp/qstar.lua" check > "$tmp/collision.out" 2> "$tmp/collision.err"; then
	fail "duplicate generated output unexpectedly succeeded"
fi
contains "$tmp/collision.err" "multiple producers"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.custom_target "bad_out" {
  outputs = {qstar.output("../bad.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}
EOF

if "$qstar" --file "$tmp/qstar.lua" check > "$tmp/outside.out" 2> "$tmp/outside.err"; then
	fail "outside generated output unexpectedly succeeded"
fi
contains "$tmp/outside.err" "must be package-relative"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.project {
  name = "generated-root",
  root = ".",
  generated_dir = "build/qstar/generated",
}

qstar.custom_target "bad_generated_dir" {
  outputs = {qstar.output("generated/safe.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}
EOF

if "$qstar" --file "$tmp/qstar.lua" check > "$tmp/generated-dir.out" 2> "$tmp/generated-dir.err"; then
	fail "generated_dir escape output unexpectedly succeeded"
fi
contains "$tmp/generated-dir.err" "must be under generated_dir 'build/qstar/generated'"
contains "$tmp/generated-dir.err" "change the output path to 'build/qstar/generated/<file>'"

if "$qstar" --file tests/corpus/bad-group/qstar.lua check > "$tmp/bad-group-corpus.out" 2> "$tmp/bad-group-corpus.err"; then
	fail "bad-group corpus unexpectedly succeeded"
fi
contains "$tmp/bad-group-corpus.err" "qstar.target_file cannot reference group target '//:aggregate' because group targets have no artifact"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.custom_target "bad_arg" {
  outputs = {qstar.output("generated/safe.c")},
  command = qstar.cli {"tools/gen-value.sh", "../escape.c"},
}

qstar.executable "bad_gen" {
  sources = {qstar.output("generated/safe.c")},
}
EOF

if "$qstar" --file "$tmp/qstar.lua" build //:bad_gen > "$tmp/bad-arg.out" 2> "$tmp/bad-arg.err"; then
	fail "generated action outside arg unexpectedly succeeded"
fi
contains "$tmp/bad-arg.err" "escapes package root"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.executable "bad_suffix" {
  sources = {"src/main.txt"},
}
EOF
cat > "$tmp/src/main.txt" <<'EOF'
not source
EOF

if "$qstar" --file "$tmp/qstar.lua" check //:bad_suffix > "$tmp/suffix.out" 2> "$tmp/suffix.err"; then
	fail "unsupported suffix unexpectedly succeeded"
fi
contains "$tmp/suffix.err" "unsupported source extension"
contains "$tmp/suffix.err" "qstar.custom_target"
contains "$tmp/suffix.err" "qstar.output(..., {format = \"object\"})"

if "$qstar" --file tests/corpus/bad-unsupported-source/qstar.lua check > "$tmp/bad-unsupported-source.out" 2> "$tmp/bad-unsupported-source.err"; then
	fail "bad unsupported source corpus unexpectedly succeeded"
fi
contains "$tmp/bad-unsupported-source.err" "Objective-C provider is not available"
contains "$tmp/bad-unsupported-source.err" "qstar.output(..., {format = \"object\"})"

for unsupported_suffix in mm rs zig swift; do
	cat > "$tmp/qstar.lua" <<EOF
qstar.executable "bad_${unsupported_suffix}" {
  sources = {"src/foreign.${unsupported_suffix}"},
}
EOF
	if "$qstar" --file "$tmp/qstar.lua" check "//:bad_${unsupported_suffix}" > "$tmp/unsupported-${unsupported_suffix}.out" 2> "$tmp/unsupported-${unsupported_suffix}.err"; then
		fail "unsupported ${unsupported_suffix} suffix unexpectedly succeeded"
	fi
	contains "$tmp/unsupported-${unsupported_suffix}.err" "unsupported source extension"
	contains "$tmp/unsupported-${unsupported_suffix}.err" "qstar.custom_target"
	contains "$tmp/unsupported-${unsupported_suffix}.err" "qstar.output(..., {format = \"object\"})"
done
contains "$tmp/unsupported-mm.err" "Objective-C++ provider is not available"
contains "$tmp/unsupported-rs.err" "this language is not a QStar compile provider"

cat > "$tmp/tools/cale" <<'EOF'
#!/bin/sh
set -eu
mode=link
out=
src=
prev=
for arg in "$@"; do
  if [ "$prev" = "-o" ]; then
    out=$arg
    prev=
    continue
  fi
  case "$arg" in
    -c) mode=compile ;;
    -o) prev="-o" ;;
    --target=*) ;;
    -*) ;;
    *) src=$arg ;;
  esac
done
if [ "$mode" = "compile" ]; then
  case "$src" in
    *.cale)
      tmp=${TMPDIR:-/tmp}/qstar-fake-cale.$$.c
      printf '%s\n' 'int cale_unit(void) { return 7; }' > "$tmp"
      cc -c "$tmp" -o "$out"
      rm -f "$tmp"
      ;;
    *)
      cc -c "$src" -o "$out"
      ;;
  esac
else
  cc "$@"
fi
EOF
chmod +x "$tmp/tools/cale"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.executable "mixed" {
  toolchain = "cale",
  sources = {"src/main.c", "src/unit.cale"},
}

qstar.staticlib "calelib" {
  toolchain = "cale",
  sources = {"src/unit.cale"},
}
EOF
cat > "$tmp/src/main.c" <<'EOF'
int cale_unit(void);
int main(void) { return cale_unit() - 7; }
EOF
cat > "$tmp/src/unit.cale" <<'EOF'
fn cale_unit() -> int { return 7; }
EOF

PATH="$tmp/tools:$PATH" "$qstar" --file "$tmp/qstar.lua" dry-run //:mixed > "$tmp/mixed-dry.out" 2> "$tmp/mixed-dry.err"
contains "$tmp/mixed-dry.out" "argv=[cale, -c, src/main.c"
contains "$tmp/mixed-dry.out" "argv=[cale, -c, src/unit.cale"
PATH="$tmp/tools:$PATH" "$qstar" --file "$tmp/qstar.lua" build //:mixed > "$tmp/mixed-build.out" 2> "$tmp/mixed-build.err"
contains "$tmp/mixed-build.out" "status ok"
PATH="$tmp/tools:$PATH" "$qstar" --file "$tmp/qstar.lua" build //:calelib > "$tmp/cale-only.out" 2> "$tmp/cale-only.err"
contains "$tmp/cale-only.out" "status ok"
contains "$tmp/build/qstar/compile_commands.json" "src/unit.cale"

if PATH="$tmp/tools:$PATH" "$qstar" --file "$tmp/qstar.lua" -G ninja build //:calelib > "$tmp/cale-ninja.out" 2> "$tmp/cale-ninja.err"; then
	fail "Cale source Ninja lowering unexpectedly succeeded"
fi
contains "$tmp/cale-ninja.err" "Cale source 'src/unit.cale' is a Stella-only language-provider action"
contains "$tmp/cale-ninja.err" "Ninja wrapper lowering is deferred"
contains "$tmp/cale-ninja.err" "use -G stella"

if PATH=/nonexistent "$qstar" --file "$tmp/qstar.lua" build //:mixed > "$tmp/no-cale.out" 2> "$tmp/no-cale.err"; then
	fail "missing cale compiler unexpectedly succeeded"
fi
contains "$tmp/no-cale.err" "Cale compiler 'cale' not found"

mkdir -p "$tmp/include" "$tmp/src/core_private" "$tmp/lib"
cat > "$tmp/include/core.h" <<'EOF'
int core_value(void);
EOF
cat > "$tmp/src/core_private/core_private.h" <<'EOF'
#define CORE_PRIVATE_VALUE 13
EOF
cat > "$tmp/src/core.c" <<'EOF'
#include "core_private.h"
int core_value(void) { return CORE_PRIVATE_VALUE; }
EOF
cat > "$tmp/src/util.c" <<'EOF'
#include "core.h"
int util_value(void) { return core_value(); }
EOF
cat > "$tmp/src/link_main.c" <<'EOF'
#include "core.h"
int util_value(void);
int main(void) { return util_value() - core_value(); }
EOF
cat > "$tmp/src/plugin_main.c" <<'EOF'
#include "core.h"
int main(void) { return core_value() - 13; }
EOF
cat > "$tmp/src/bad_private.c" <<'EOF'
#include "core_private.h"
int main(void) { return CORE_PRIVATE_VALUE; }
EOF
cat > "$tmp/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = {
      public_headers = {"include/core.h"},
      public_include_dirs = {"include"},
      private_include_dirs = {"src/core_private"},
    },
  },
}

qstar.staticlib "util" {
  sources = {"src/util.c"},
  deps = {"//:core"},
}

qstar.executable "linkapp" {
  sources = {"src/link_main.c"},
  deps = {"//:util"},
}

qstar.executable "bad_private" {
  sources = {"src/bad_private.c"},
  deps = {"//:core"},
}

qstar.executable "sysflags" {
  sources = {"src/link_main.c"},
  libs = {"m"},
  lib_dirs = {"lib"},
}

qstar.sharedlib "plugin" {
  sources = {"src/core.c"},
  lang = {
    c = {
      public_headers = {"include/core.h"},
      public_include_dirs = {"include"},
      private_include_dirs = {"src/core_private"},
    },
  },
}

qstar.executable "plugin_app" {
  sources = {"src/plugin_main.c"},
  deps = {"//:plugin"},
}

qstar.stage "shared_bundle" {
  root = "stage/shared",
  files = {
    qstar.stage_file(qstar.target_file("//:plugin"), "lib/plugin.shared"),
  },
}

qstar.profile "windows-shared" {
  target = "x86_64-pc-windows-msvc",
  toolchain = "clang",
}
EOF

"$qstar" --file "$tmp/qstar.lua" build //:linkapp > "$tmp/linkapp.out" 2> "$tmp/linkapp.err"
contains "$tmp/linkapp.out" "status ok"
not_contains "$tmp/linkapp.out" "ranlib: warning"
not_contains "$tmp/linkapp.out" "archive member"
not_contains "$tmp/linkapp.err" "ranlib: warning"
not_contains "$tmp/linkapp.err" "archive member"
"$qstar" --file "$tmp/qstar.lua" action-log //:linkapp:link:0 > "$tmp/linkapp-log.out" 2> "$tmp/linkapp-log.err"
case "$(cat "$tmp/linkapp-log.out")" in
  *libutil.a*libcore.a*) ;;
  *) fail "link order did not include util before core" ;;
esac
"$qstar" --file "$tmp/qstar.lua" action-log //:util:archive:0 > "$tmp/util-archive-log.out" 2> "$tmp/util-archive-log.err"
contains "$tmp/util-archive-log.out" "argv[0]=ar"
contains "$tmp/util-archive-log.out" "libutil.a"
contains "$tmp/util-archive-log.out" "obj0.o"
not_contains "$tmp/util-archive-log.out" "libcore.a"
"$qstar" --file "$tmp/qstar.lua" replay //:util:archive:0 > "$tmp/util-archive-replay.out" 2> "$tmp/util-archive-replay.err"
contains "$tmp/util-archive-replay.out" "qstar replay v1"
contains "$tmp/util-archive-replay.out" "ar rcs"
contains "$tmp/util-archive-replay.out" "libutil.a"
"$qstar" --file "$tmp/qstar.lua" emit-ninja //:util > "$tmp/util-emit-ninja.out" 2> "$tmp/util-emit-ninja.err"
contains "$tmp/build/qstar/ninja/build.ninja" "build build/qstar/out/___util/libutil.a: qstar_archive build/qstar/out/___util/obj0.o ||"
not_contains "$tmp/build/qstar/ninja/build.ninja" "libutil.a: qstar_archive build/qstar/out/___util/obj0.o build/qstar/out/___core/libcore.a"

if "$qstar" --file "$tmp/qstar.lua" build //:bad_private > "$tmp/bad-private.out" 2> "$tmp/bad-private.err"; then
	fail "private include propagation unexpectedly succeeded"
fi
contains "$tmp/bad-private.err" "action '//:bad_private:compile:0' failed"

"$qstar" --file "$tmp/qstar.lua" dry-run //:sysflags > "$tmp/sysflags.out" 2> "$tmp/sysflags.err"
contains "$tmp/sysflags.out" "-Llib"
contains "$tmp/sysflags.out" "-lm"

case "$(uname -s)" in
	Darwin)
		shared_artifact="build/qstar/out/___plugin/libplugin.dylib"
		shared_flag="-dynamiclib"
		shared_name_flag="@rpath/libplugin.dylib"
		shared_rpath_flag="-Wl,-rpath,@loader_path/../___plugin"
		shared_ninja_rpath_flag="-Wl,-rpath,@loader_path/../___plugin"
		;;
	Linux)
		shared_artifact="build/qstar/out/___plugin/libplugin.so"
		shared_flag="-shared"
		shared_name_flag="-Wl,-soname,libplugin.so"
		shared_rpath_flag='-Wl,-rpath,$ORIGIN/../___plugin'
		shared_ninja_rpath_flag='-Wl,-rpath,$$ORIGIN/../___plugin'
		;;
	*)
		shared_artifact="build/qstar/out/___plugin/libplugin.so"
		shared_flag="-shared"
		shared_name_flag="-Wl,-soname,libplugin.so"
		shared_rpath_flag='-Wl,-rpath,$ORIGIN/../___plugin'
		shared_ninja_rpath_flag='-Wl,-rpath,$$ORIGIN/../___plugin'
		;;
esac

"$qstar" --file "$tmp/qstar.lua" dry-run //:plugin > "$tmp/shared-dry.out" 2> "$tmp/shared-dry.err"
contains "$tmp/shared-dry.out" "final_action=link-shared"
contains "$tmp/shared-dry.out" "$shared_artifact"
contains "$tmp/shared-dry.out" "-fPIC"
contains "$tmp/shared-dry.out" "$shared_flag"
contains "$tmp/shared-dry.out" "$shared_name_flag"
"$qstar" --file "$tmp/qstar.lua" build //:plugin --progress off > "$tmp/shared.out" 2> "$tmp/shared.err"
contains "$tmp/shared.out" "status ok"
test -f "$tmp/$shared_artifact" || fail "sharedlib artifact missing"
"$qstar" --file "$tmp/qstar.lua" action-log //:plugin:link-shared:0 > "$tmp/shared-log.out" 2> "$tmp/shared-log.err"
contains "$tmp/shared-log.out" "description='Linking C shared library $shared_artifact'"
contains "$tmp/shared-log.out" "$shared_flag"
contains "$tmp/shared-log.out" "status ok"
"$qstar" --file "$tmp/qstar.lua" build //:plugin_app --progress off > "$tmp/shared-app.out" 2> "$tmp/shared-app.err"
contains "$tmp/shared-app.out" "status ok"
"$tmp/build/qstar/out/___plugin_app/plugin_app"
"$qstar" --file "$tmp/qstar.lua" action-log //:plugin_app:link:0 > "$tmp/shared-app-log.out" 2> "$tmp/shared-app-log.err"
contains "$tmp/shared-app-log.out" "$shared_artifact"
contains "$tmp/shared-app-log.out" "$shared_rpath_flag"
"$qstar" --file "$tmp/qstar.lua" stage //:shared_bundle > "$tmp/shared-stage.out" 2> "$tmp/shared-stage.err"
contains "$tmp/shared-stage.out" "status ok"
contains "$tmp/shared-stage.out" "stage_file src=$shared_artifact dst=stage/shared/lib/plugin.shared mode=copy kind=target producer=//:plugin"
test -f "$tmp/stage/shared/lib/plugin.shared" || fail "sharedlib stage artifact missing"
contains "$tmp/build/qstar/stage/___shared_bundle/manifest.json" "\"producer\":\"//:plugin\""
"$qstar" --file "$tmp/qstar.lua" install //:plugin --prefix "$tmp/shared-prefix" > "$tmp/shared-install.out" 2> "$tmp/shared-install.err"
contains "$tmp/shared-install.out" "status ok"
test -f "$tmp/shared-prefix/lib/$(basename "$shared_artifact")" || fail "sharedlib install artifact missing"
test -f "$tmp/shared-prefix/include/core.h" || fail "sharedlib install header missing"
contains "$tmp/build/qstar/install/manifest.json" "\"role\":\"sharedlib\""
"$qstar" --file "$tmp/qstar.lua" emit-ninja //:plugin_app > "$tmp/shared-ninja-emit.out" 2> "$tmp/shared-ninja-emit.err"
contains "$tmp/build/qstar/ninja/build.ninja" "qstar_action_id = //:plugin:link-shared:0"
contains "$tmp/build/qstar/ninja/build.ninja" "qstar_action_id = //:plugin_app:link:0"
contains "$tmp/build/qstar/ninja/build.ninja" "description = Linking C shared library $shared_artifact"
contains "$tmp/build/qstar/ninja/build.ninja" "$shared_flag"
contains "$tmp/build/qstar/ninja/build.ninja" "$shared_name_flag"
contains "$tmp/build/qstar/ninja/build.ninja" "$shared_ninja_rpath_flag"
if command -v ninja >/dev/null 2>&1; then
	"$qstar" --file "$tmp/qstar.lua" -G ninja build //:plugin_app --progress off > "$tmp/shared-ninja.out" 2> "$tmp/shared-ninja.err"
	contains "$tmp/shared-ninja.out" "backend ninja"
	contains "$tmp/shared-ninja.out" "status ok"
	test -f "$tmp/$shared_artifact" || fail "sharedlib ninja artifact missing"
	test -f "$tmp/build/qstar/out/___plugin_app/plugin_app" || fail "sharedlib ninja app missing"
	"$tmp/build/qstar/out/___plugin_app/plugin_app"
	test ! -f "$tmp/.ninja_log" || fail "sharedlib ninja wrote package root .ninja_log"
	test ! -f "$tmp/.ninja_deps" || fail "sharedlib ninja wrote package root .ninja_deps"
fi
if "$qstar" --file "$tmp/qstar.lua" --profile windows-shared build //:plugin > "$tmp/shared-windows.out" 2> "$tmp/shared-windows.err"; then
	fail "windows sharedlib unexpectedly succeeded"
fi
contains "$tmp/shared-windows.err" "Windows .dll/import-library policy is deferred"
if "$qstar" --file "$tmp/qstar.lua" --profile windows-shared -G ninja build //:plugin > "$tmp/shared-windows-ninja.out" 2> "$tmp/shared-windows-ninja.err"; then
	fail "windows sharedlib ninja unexpectedly succeeded"
fi
contains "$tmp/shared-windows-ninja.err" "Windows .dll/import-library policy is deferred"

cat > "$tmp/src/test_pass.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/src/test_fail.c" <<'EOF'
int main(void) { return 3; }
EOF
cat > "$tmp/src/test_timeout.c" <<'EOF'
int main(void) { for (;;) {} return 0; }
EOF
cat > "$tmp/src/install_main.c" <<'EOF'
#include "core.h"
int main(void) { return core_value() - 13; }
EOF
cat > "$tmp/qstar.lua" <<'EOF'
qstar.configure_file "install_cfg" {
  output = qstar.output("generated/install_config.h"),
  defines = {"INSTALL_FEATURE=1"},
}

qstar.test "unit_pass" {
  sources = {"src/test_pass.c"},
}

qstar.test "unit_fail" {
  sources = {"src/test_fail.c"},
}

qstar.test "unit_timeout" {
  sources = {"src/test_timeout.c"},
}

qstar.staticlib "install_core" {
  sources = {"src/core.c"},
  lang = {
    c = {
      public_headers = {"include/core.h", qstar.output("generated/install_config.h")},
      public_include_dirs = {"include"},
      private_include_dirs = {"src/core_private"},
    },
  },
}

qstar.executable "install_app" {
  sources = {"src/install_main.c"},
  deps = {"//:install_core"},
}
EOF

"$qstar" --file "$tmp/qstar.lua" test //:unit_pass > "$tmp/test-pass.out" 2> "$tmp/test-pass.err"
contains "$tmp/test-pass.out" "test_result label=//:unit_pass status=pass"
test -f "$tmp/build/qstar/logs/___unit_pass.test.stdout" || fail "missing test stdout log"

if "$qstar" --file "$tmp/qstar.lua" test //:unit_fail > "$tmp/test-fail.out" 2> "$tmp/test-fail.err"; then
	fail "failing test unexpectedly succeeded"
fi
contains "$tmp/test-fail.out" "test_result label=//:unit_fail status=fail exit=3"

if "$qstar" --file "$tmp/qstar.lua" test //:unit_timeout > "$tmp/test-timeout.out" 2> "$tmp/test-timeout.err"; then
	fail "timeout test unexpectedly succeeded"
fi
contains "$tmp/test-timeout.out" "test_result label=//:unit_timeout status=timeout"

"$qstar" --file "$tmp/qstar.lua" build //:install_app > "$tmp/install-build.out" 2> "$tmp/install-build.err"
contains "$tmp/install-build.out" "status ok"
"$qstar" --file "$tmp/qstar.lua" install //:install_app --prefix "$tmp/prefix" --dry-run > "$tmp/install-dry.out" 2> "$tmp/install-dry.err"
contains "$tmp/install-dry.out" "mode dry-run"
contains "$tmp/install-dry.out" "install_file src=build/qstar/out/___install_app/install_app"
contains "$tmp/install-dry.out" "description=\"Installing build/qstar/out/___install_app/install_app\""
contains "$tmp/install-dry.out" "install_diff dst=$tmp/prefix/bin/install_app action=would-create"
"$qstar" --file "$tmp/qstar.lua" install //:install_core --prefix "$tmp/prefix" > "$tmp/install-lib.out" 2> "$tmp/install-lib.err"
contains "$tmp/install-lib.out" "status ok"
test -f "$tmp/prefix/lib/libinstall_core.a" || fail "missing installed staticlib"
test -f "$tmp/prefix/include/core.h" || fail "missing installed public header"
test -f "$tmp/prefix/include/generated/install_config.h" || fail "missing installed generated public header"
test -f "$tmp/build/qstar/install/manifest.json" || fail "missing install manifest"
contains "$tmp/build/qstar/install/manifest.json" "\"schema\":\"qstar-install-manifest-v2\""
contains "$tmp/build/qstar/install/manifest.json" "\"role\":\"staticlib\""
contains "$tmp/build/qstar/install/manifest.json" "\"role\":\"header\""
contains "$tmp/build/qstar/install/manifest.json" "\"cmake_config\":\"deferred\""

if "$qstar" --file "$tmp/qstar.lua" install //:unit_pass --prefix "$tmp/prefix" > "$tmp/install-test.out" 2> "$tmp/install-test.err"; then
	fail "non-installable test target unexpectedly installed"
fi
contains "$tmp/install-test.err" "not installable"

manual_root=$(pwd)/tests/manual
project_root=$(pwd)/tests/projects

cp -R "$manual_root/c-only" "$tmp/c-only"
"$qstar" --file "$tmp/c-only/qstar.lua" build //:app > "$tmp/c-only-build.out" 2> "$tmp/c-only-build.err"
contains "$tmp/c-only-build.out" "status ok"
"$tmp/c-only/build/qstar/out/___app/app"
contains "$tmp/c-only/build/qstar/compile_commands.json" "src/main.c"
contains "$tmp/c-only/build/qstar/compile_commands.json" "src/core.c"
"$qstar" --file "$tmp/c-only/qstar.lua" test //:unit > "$tmp/c-only-test.out" 2> "$tmp/c-only-test.err"
contains "$tmp/c-only-test.out" "test_result label=//:unit status=pass"
"$qstar" --file "$tmp/c-only/qstar.lua" install //:core --prefix "$tmp/c-only-prefix" > "$tmp/c-only-install.out" 2> "$tmp/c-only-install.err"
contains "$tmp/c-only-install.out" "status ok"
test -f "$tmp/c-only-prefix/lib/libcore.a" || fail "c-only sample did not install libcore.a"
test -f "$tmp/c-only-prefix/include/corpus.h" || fail "c-only sample did not install public header"
contains "$tmp/c-only/build/qstar/compile_commands.json" "tests/unit.c"
"$qstar" --file "$tmp/c-only/qstar.lua" clean > "$tmp/c-only-clean.out" 2> "$tmp/c-only-clean.err"
contains "$tmp/c-only-clean.out" "clean_all build/qstar compile_commands=build"
"$qstar" --file "$tmp/c-only/qstar.lua" build //:app --verbose > "$tmp/c-only-rebuild.out" 2> "$tmp/c-only-rebuild.err"
contains "$tmp/c-only-rebuild.out" "status=run"
contains "$tmp/c-only-rebuild.out" "status ok"

cp -R "$manual_root/generated" "$tmp/generated-sample"
rm -rf "$tmp/generated-sample/build/qstar" "$tmp/generated-sample/generated" "$tmp/generated-sample/build/qstar/compile_commands.json"
"$qstar" --file "$tmp/generated-sample/qstar.lua" build //:app > "$tmp/generated-sample-build.out" 2> "$tmp/generated-sample-build.err"
contains "$tmp/generated-sample-build.out" "status ok"
test -f "$tmp/generated-sample/generated/config.h" || fail "generated sample missing config header"
test -f "$tmp/generated-sample/generated/value.c" || fail "generated sample missing generated source"
"$tmp/generated-sample/build/qstar/out/___app/app"
contains "$tmp/generated-sample/build/qstar/compile_commands.json" "src/main.c"
contains "$tmp/generated-sample/build/qstar/compile_commands.json" "generated/value.c"
"$qstar" --file "$tmp/generated-sample/qstar.lua" list-targets --format json > "$tmp/generated-sample-targets.out" 2> "$tmp/generated-sample-targets.err"
contains "$tmp/generated-sample-targets.out" "\"schema\":\"qstar-targets-v1\""
contains "$tmp/generated-sample-targets.out" "\"generated_actions\":["
contains "$tmp/generated-sample-targets.out" "\"config_header\":true"
contains "$tmp/generated-sample-targets.out" "\"label\":\"//:generated_value\""
"$qstar" --file "$tmp/generated-sample/qstar.lua" clean > "$tmp/generated-sample-clean.out" 2> "$tmp/generated-sample-clean.err"
"$qstar" --file "$tmp/generated-sample/qstar.lua" build //:app > "$tmp/generated-sample-rebuild.out" 2> "$tmp/generated-sample-rebuild.err"
contains "$tmp/generated-sample-rebuild.out" "status ok"

cp -R "$manual_root/mixed-cale" "$tmp/mixed-sample"
"$qstar" --file "$tmp/mixed-sample/qstar.lua" dry-run //:mixed > "$tmp/mixed-sample-dry.out" 2> "$tmp/mixed-sample-dry.err"
contains "$tmp/mixed-sample-dry.out" "argv=[cale, -c, src/main.c"
contains "$tmp/mixed-sample-dry.out" "argv=[cale, -c, src/plugin.cale"
contains "$tmp/mixed-sample-dry.out" "rule provider=native final_action=link output_group=exe"

cp -R "$project_root/c-app-lib-test" "$tmp/project-c"
"$qstar" --file "$tmp/project-c/qstar.lua" build //:app > "$tmp/project-c-build.out" 2> "$tmp/project-c-build.err"
contains "$tmp/project-c-build.out" "status ok"
"$tmp/project-c/build/qstar/out/___app/app"
"$qstar" --file "$tmp/project-c/qstar.lua" test //:unit > "$tmp/project-c-test.out" 2> "$tmp/project-c-test.err"
contains "$tmp/project-c-test.out" "test_result label=//:unit status=pass"
"$qstar" --file "$tmp/project-c/qstar.lua" install //:core --prefix "$tmp/project-c-prefix" > "$tmp/project-c-install.out" 2> "$tmp/project-c-install.err"
test -f "$tmp/project-c-prefix/lib/libcore.a" || fail "project corpus c lib did not install"
test -f "$tmp/project-c-prefix/include/corpus.h" || fail "project corpus c header did not install"
contains "$tmp/project-c/build/qstar/install/manifest.json" "\"schema\":\"qstar-install-manifest-v2\""
contains "$tmp/project-c/build/qstar/install/manifest.json" "\"role\":\"staticlib\""
contains "$tmp/project-c/build/qstar/install/manifest.json" "\"cmake_config\":\"deferred\""
contains "$tmp/project-c/build/qstar/compile_commands.json" "src/core.c"
contains "$tmp/project-c/build/qstar/compile_commands.json" "tests/unit.c"
test ! -e "$tmp/project-c/build/qstar/state/graph.json" || fail "project corpus graph snapshot should be debug opt-in"
test ! -e "$tmp/project-c/build/qstar/state/last-summary.json" || fail "project corpus success summary should be debug opt-in"
"$qstar" --file "$tmp/project-c/qstar.lua" build //:app --verbose > "$tmp/project-c-skip.out" 2> "$tmp/project-c-skip.err"
contains "$tmp/project-c-skip.out" "status=skip"
cat > "$tmp/project-c/src/main.c" <<'EOF'
#include "corpus.h"
int main(void) { return corpus_value() - 31; }
EOF
"$qstar" --file "$tmp/project-c/qstar.lua" build //:app --explain-cache > "$tmp/project-c-rebuild.out" 2> "$tmp/project-c-rebuild.err"
contains "$tmp/project-c-rebuild.out" "cache_miss id=//:app:compile:0"
contains "$tmp/project-c-rebuild.out" "status ok"

if command -v c++ >/dev/null 2>&1; then
	cp -R "$project_root/cxx-mixed" "$tmp/project-cxx"
	"$qstar" --file "$tmp/project-cxx/qstar.lua" build //:mixed --jobs 2 --schedule-trace > "$tmp/project-cxx-build.out" 2> "$tmp/project-cxx-build.err"
	contains "$tmp/project-cxx-build.out" "parallel_compile target=//:mixed jobs=2 sources=2 mode=process-v3"
	contains "$tmp/project-cxx-build.out" "parallel_batch target=//:mixed jobs=2 total=2 policy=fifo"
	contains "$tmp/project-cxx-build.out" "schedule_action id=//:mixed:compile:0"
	contains "$tmp/project-cxx-build.out" "schedule_action id=//:mixed:compile:1"
	contains "$tmp/project-cxx-build.out" "parallel_event target=//:mixed event=start id=//:mixed:compile:0"
	contains "$tmp/project-cxx-build.out" "parallel_event target=//:mixed event=finish"
	contains "$tmp/project-cxx-build.out" "status ok"
	"$tmp/project-cxx/build/qstar/out/___mixed/mixed"
	contains "$tmp/project-cxx/build/qstar/compile_commands.json" "src/cpp.cpp"
	contains "$tmp/project-cxx/build/qstar/compile_commands.json" "src/main.c"
fi

mkdir -p "$tmp/fanout/src"
cat > "$tmp/fanout/src/a.c" <<'EOF'
#include "config.h"
int a_value(void) { return FANOUT_VALUE; }
EOF
cat > "$tmp/fanout/src/b.c" <<'EOF'
#include "config.h"
int b_value(void) { return FANOUT_VALUE + 1; }
EOF
cat > "$tmp/fanout/src/main.c" <<'EOF'
int a_value(void);
int b_value(void);
int main(void) { return a_value() + b_value() - 15; }
EOF
cat > "$tmp/fanout/qstar.lua" <<'EOF'
qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"FANOUT_VALUE=7"},
}

qstar.executable "app" {
  sources = {"src/a.c", "src/b.c", "src/main.c"},
  lang = {
    c = {
      private_headers = {qstar.output("generated/config.h")},
      include_dirs = {"generated"},
    },
  },
}
EOF
"$qstar" --file "$tmp/fanout/qstar.lua" build //:app --jobs 2 --schedule-trace > "$tmp/fanout-build.out" 2> "$tmp/fanout-build.err"
contains "$tmp/fanout-build.out" "build_action id=//:cfg:generate:0 status=run"
contains "$tmp/fanout-build.out" "parallel_compile target=//:app jobs=2 sources=3 mode=process-v3"
contains "$tmp/fanout-build.out" "parallel_slot target=//:app slot=0 state=assign action=//:app:compile:0 queue=0"
contains "$tmp/fanout-build.out" "parallel_slot target=//:app slot=1 state=assign action=//:app:compile:1 queue=1"
contains "$tmp/fanout-build.out" "action=//:app:compile:2 queue=2"
contains "$tmp/fanout-build.out" "status ok"

mkdir -p "$tmp/cross-target/src" "$tmp/cross-target/tools"
cat > "$tmp/cross-target/tools/fake-cc.sh" <<'EOF'
#!/bin/sh
set -eu
exec cc "$@"
EOF
chmod +x "$tmp/cross-target/tools/fake-cc.sh"
cat > "$tmp/cross-target/src/a.c" <<'EOF'
int a_value(void) { return 1; }
EOF
cat > "$tmp/cross-target/src/b.c" <<'EOF'
int b_value(void) { return 2; }
EOF
cat > "$tmp/cross-target/qstar.lua" <<'EOF'
qstar.profile "default" {
  cc = "tools/fake-cc.sh",
}

qstar.staticlib "liba" {
  sources = {"src/a.c"},
}

qstar.staticlib "libb" {
  sources = {"src/b.c"},
}

qstar.group "all" {
  deps = {
    "//:liba",
    "//:libb",
  },
}
EOF
"$qstar" --file "$tmp/cross-target/qstar.lua" build //:all --jobs 2 --schedule-trace > "$tmp/cross-target-build.out" 2> "$tmp/cross-target-build.err"
contains "$tmp/cross-target-build.out" "action_scheduler version=v1"
contains "$tmp/cross-target-build.out" "parallel_slot target=//:liba slot=0 state=assign action=//:liba:compile:0 queue=0 scheduler=global"
contains "$tmp/cross-target-build.out" "parallel_slot target=//:libb slot=1 state=assign action=//:libb:compile:0 queue=0 scheduler=global"
contains "$tmp/cross-target-build.out" "group_target label=//:all"
contains "$tmp/cross-target-build.out" "status ok"

mkdir -p "$tmp/parallel-fail/src" "$tmp/parallel-fail/tools"
cat > "$tmp/parallel-fail/tools/fake-cc.sh" <<'EOF'
#!/bin/sh
set -eu
src=
out=
dep=
prev=
for arg in "$@"; do
  if [ "$prev" = "-o" ]; then out=$arg; prev=; continue; fi
  if [ "$prev" = "-MF" ]; then dep=$arg; prev=; continue; fi
  case "$arg" in
    -o) prev="-o" ;;
    -MF) prev="-MF" ;;
    *.c) src=$arg ;;
  esac
done
case "$src" in
  *slow.c) sleep 20 ;;
  *fail.c) echo "fake compiler failure" >&2; exit 9 ;;
esac
if [ "$dep" ]; then
  mkdir -p "$(dirname "$dep")"
  printf '%s: %s\n' "$out" "$src" > "$dep"
fi
cc -c "$src" -o "$out"
EOF
chmod +x "$tmp/parallel-fail/tools/fake-cc.sh"
cat > "$tmp/parallel-fail/src/slow.c" <<'EOF'
int slow_value(void) { return 1; }
EOF
cat > "$tmp/parallel-fail/src/fail.c" <<'EOF'
int fail_value(void) { return 2; }
EOF
cat > "$tmp/parallel-fail/src/ok.c" <<'EOF'
int ok_value(void) { return 3; }
EOF
cat > "$tmp/parallel-fail/qstar.lua" <<'EOF'
qstar.profile "default" {
  cc = "tools/fake-cc.sh",
}

qstar.executable "race" {
  sources = {"src/slow.c", "src/fail.c", "src/ok.c"},
}
EOF
if "$qstar" --file "$tmp/parallel-fail/qstar.lua" build //:race --jobs 2 --schedule-trace > "$tmp/parallel-fail.out" 2> "$tmp/parallel-fail.err"; then
	fail "parallel failure unexpectedly succeeded"
fi
contains "$tmp/parallel-fail.out" "parallel_compile target=//:race jobs=2 sources=3 mode=process-v3"
contains "$tmp/parallel-fail.out" "parallel_batch target=//:race jobs=2 total=3 policy=fifo"
contains "$tmp/parallel-fail.out" "parallel_slot target=//:race slot=0 state=assign action=//:race:compile:0 queue=0"
contains "$tmp/parallel-fail.out" "parallel_slot target=//:race slot=1 state=assign action=//:race:compile:1 queue=1"
contains "$tmp/parallel-fail.out" "build_action id=//:race:compile:1 status=fail exit=9"
contains "$tmp/parallel-fail.out" "parallel_event target=//:race event=fail id=//:race:compile:1 slot=1 exit=9 state=failed retry=next-build cancel=active"
contains "$tmp/parallel-fail.out" "build_action id=//:race:compile:0 status=cancelled reason=parallel-failure retry=next-build"
contains "$tmp/parallel-fail.out" "parallel_event target=//:race event=cancel id=//:race:compile:0 slot=0 state=cancelled reason=parallel-failure retry=next-build"
contains "$tmp/parallel-fail/build/qstar/logs/___race_compile_1.log" "qstar-action-log v2"
contains "$tmp/parallel-fail/build/qstar/logs/___race_compile_1.log" "argv[0]=tools/fake-cc.sh"
contains "$tmp/parallel-fail/build/qstar/logs/last-failure.replay" "argv_digest="

mkdir -p "$tmp/parallel-timeout/src" "$tmp/parallel-timeout/tools"
cat > "$tmp/parallel-timeout/tools/fake-cc.sh" <<'EOF'
#!/bin/sh
set -eu
sleep 5
EOF
chmod +x "$tmp/parallel-timeout/tools/fake-cc.sh"
cat > "$tmp/parallel-timeout/src/timeout.c" <<'EOF'
int timeout_value(void) { return 1; }
EOF
cat > "$tmp/parallel-timeout/src/other.c" <<'EOF'
int other_value(void) { return 2; }
EOF
cat > "$tmp/parallel-timeout/qstar.lua" <<'EOF'
qstar.profile "default" {
  cc = "tools/fake-cc.sh",
}

qstar.executable "timeout" {
  sources = {"src/timeout.c", "src/other.c"},
}
EOF
if QSTAR_TEST_ACTION_TIMEOUT_SEC=1 "$qstar" --file "$tmp/parallel-timeout/qstar.lua" build //:timeout --jobs 2 --schedule-trace > "$tmp/parallel-timeout.out" 2> "$tmp/parallel-timeout.err"; then
	fail "parallel timeout unexpectedly succeeded"
fi
contains "$tmp/parallel-timeout.out" "executor-policy version=v4 parallel=optional jobs=2 active=action-dag-ready-queue failure=stop-on-first-failure action_timeout_sec=1"
contains "$tmp/parallel-timeout.out" "parallel_compile target=//:timeout jobs=2 sources=2 mode=process-v3"
contains "$tmp/parallel-timeout.out" "parallel_event target=//:timeout event=timeout id=//:timeout:compile:0 slot=0 state=timeout retry=next-build cancel=active"
contains "$tmp/parallel-timeout.out" "build_action id=//:timeout:compile:1 status=cancelled reason=parallel-failure retry=next-build"
contains "$tmp/parallel-timeout.out" "parallel_event target=//:timeout event=cancel id=//:timeout:compile:1 slot=1 state=cancelled reason=parallel-failure retry=next-build"
contains "$tmp/parallel-timeout.out" "cancel_propagation policy=stop-on-first-failure"
contains "$tmp/parallel-timeout/build/qstar/logs/last-failure.replay" "argv_digest="

cp -R "$project_root/generated-config" "$tmp/project-generated"
"$qstar" --file "$tmp/project-generated/qstar.lua" build //:app > "$tmp/project-generated-build.out" 2> "$tmp/project-generated-build.err"
contains "$tmp/project-generated-build.out" "status ok"
test -f "$tmp/project-generated/generated/config.h" || fail "project corpus generated config missing"
test -f "$tmp/project-generated/generated/value.c" || fail "project corpus generated source missing"
"$tmp/project-generated/build/qstar/out/___app/app"
contains "$tmp/project-generated/build/qstar/compile_commands.json" "generated/value.c"
test ! -e "$tmp/project-generated/build/qstar/state/graph.json" || fail "project generated graph snapshot should be debug opt-in"
"$qstar" --file "$tmp/project-generated/qstar.lua" build //:app --verbose > "$tmp/project-generated-skip.out" 2> "$tmp/project-generated-skip.err"
contains "$tmp/project-generated-skip.out" "status=skip"

cp -R "$project_root/binary-blob-embed" "$tmp/project-blob"
"$qstar" --file "$tmp/project-blob/qstar.lua" build //:probe --explain-cache --verbose > "$tmp/project-blob-build.out" 2> "$tmp/project-blob-build.err"
contains "$tmp/project-blob-build.out" "build_action id=//:embed_object:generate:0 status=run"
contains "$tmp/project-blob-build.out" "build_action id=//:probe:link:0 status=run"
contains "$tmp/project-blob-build.out" "status ok"
"$tmp/project-blob/build/qstar/out/___probe/probe"
"$qstar" --file "$tmp/project-blob/qstar.lua" build //:probe --explain-cache --verbose > "$tmp/project-blob-skip.out" 2> "$tmp/project-blob-skip.err"
contains "$tmp/project-blob-skip.out" "build_action id=//:embed_object:generate:0 status=skip"
contains "$tmp/project-blob-skip.out" "build_action id=//:probe:link:0 status=skip"
printf 'payload-v2\n' >> "$tmp/project-blob/fixtures/payload.elf"
"$qstar" --file "$tmp/project-blob/qstar.lua" build //:probe --explain-cache > "$tmp/project-blob-rebuild.out" 2> "$tmp/project-blob-rebuild.err"
contains "$tmp/project-blob-rebuild.out" "cache_miss id=//:embed_object:generate:0 reason=input-changed"
contains "$tmp/project-blob-rebuild.out" "cache_miss id=//:probe:link:0 reason=input-changed"

cp -R "$project_root/object-artifact-bridge" "$tmp/project-object-bridge"
case "$(uname -s)" in
	Darwin) object_bridge_shared="build/qstar/out/___objc_plugin/libobjc_plugin.dylib" ;;
	*) object_bridge_shared="build/qstar/out/___objc_plugin/libobjc_plugin.so" ;;
esac
"$qstar" --file "$tmp/project-object-bridge/qstar.lua" dry-run //:all > "$tmp/project-object-bridge-dry.out" 2> "$tmp/project-object-bridge-dry.err"
contains "$tmp/project-object-bridge-dry.out" "generated_artifact output=build/qstar/generated/objc/AppDelegate.o group=objects format=object"
contains "$tmp/project-object-bridge-dry.out" "dry_run_step id=//:app:link-input:1"
contains "$tmp/project-object-bridge-dry.out" "dry_run_step id=//:objc_static:link-input:0"
contains "$tmp/project-object-bridge-dry.out" "dry_run_step id=//:objc_plugin:link-input:1"
"$qstar" --file "$tmp/project-object-bridge/qstar.lua" build //:all --progress off > "$tmp/project-object-bridge-build.out" 2> "$tmp/project-object-bridge-build.err"
contains "$tmp/project-object-bridge-build.out" "status ok"
test -f "$tmp/project-object-bridge/build/qstar/generated/objc/AppDelegate.o" || fail "object bridge generated object missing"
test -f "$tmp/project-object-bridge/build/qstar/out/___app/app" || fail "object bridge executable missing"
test -f "$tmp/project-object-bridge/build/qstar/out/___objc_static/libobjc_static.a" || fail "object bridge staticlib missing"
test -f "$tmp/project-object-bridge/$object_bridge_shared" || fail "object bridge sharedlib missing"
"$tmp/project-object-bridge/build/qstar/out/___app/app"
"$qstar" --file "$tmp/project-object-bridge/qstar.lua" action-log //:objc_object:generate:0 > "$tmp/project-object-bridge-log.out" 2> "$tmp/project-object-bridge-log.err"
contains "$tmp/project-object-bridge-log.out" "description='Building Objective-C object AppDelegate.o'"
contains "$tmp/project-object-bridge-log.out" "tools/fake-objc-compile.sh"
"$qstar" --file "$tmp/project-object-bridge/qstar.lua" replay //:objc_object:generate:0 > "$tmp/project-object-bridge-replay.out" 2> "$tmp/project-object-bridge-replay.err"
contains "$tmp/project-object-bridge-replay.out" "qstar replay v1"
contains "$tmp/project-object-bridge-replay.out" "tools/fake-objc-compile.sh src/AppDelegate.m build/qstar/generated/objc/AppDelegate.o"

cp -R "$project_root/multipkg" "$tmp/project-multipkg"
"$qstar" --file "$tmp/project-multipkg/qstar.lua" build //app:app --verbose > "$tmp/project-multipkg-build.out" 2> "$tmp/project-multipkg-build.err"
contains "$tmp/project-multipkg-build.out" "package-root $tmp/project-multipkg"
contains "$tmp/project-multipkg-build.out" "status ok"
"$tmp/project-multipkg/build/qstar/out/__app_app/app"
"$qstar" --file "$tmp/project-multipkg/qstar.lua" install //lib:core --prefix "$tmp/project-multipkg-prefix" > "$tmp/project-multipkg-install.out" 2> "$tmp/project-multipkg-install.err"
test -f "$tmp/project-multipkg-prefix/lib/libcore.a" || fail "multipkg corpus lib did not install"
test -f "$tmp/project-multipkg-prefix/include/core.h" || fail "multipkg corpus header did not install"
contains "$tmp/project-multipkg/build/qstar/install/manifest.json" "\"schema\":\"qstar-install-manifest-v2\""
contains "$tmp/project-multipkg/build/qstar/compile_commands.json" "lib/src/core.c"
contains "$tmp/project-multipkg/build/qstar/compile_commands.json" "app/src/main.c"

cp -R "$project_root/source-dir-style" "$tmp/project-source-dir"
"$qstar" --file "$tmp/project-source-dir/qstar.lua" lint //... > "$tmp/project-source-dir-lint.out" 2> "$tmp/project-source-dir-lint.err"
contains "$tmp/project-source-dir-lint.out" "status ok"
"$qstar" --file "$tmp/project-source-dir/qstar.lua" explain //app/src:app > "$tmp/project-source-dir-explain.out" 2> "$tmp/project-source-dir-explain.err"
contains "$tmp/project-source-dir-explain.out" "closure-order [//lib/src:core, //app/src:app]"
"$qstar" --file "$tmp/project-source-dir/qstar.lua" dry-run //app/src:app > "$tmp/project-source-dir-dry.out" 2> "$tmp/project-source-dir-dry.err"
contains "$tmp/project-source-dir-dry.out" "dry_run_target //app/src:app"
"$qstar" --file "$tmp/project-source-dir/qstar.lua" build //app/src:app > "$tmp/project-source-dir-build.out" 2> "$tmp/project-source-dir-build.err"
contains "$tmp/project-source-dir-build.out" "status ok"
"$tmp/project-source-dir/build/qstar/out/__app_src_app/app"
contains "$tmp/project-source-dir/build/qstar/compile_commands.json" "lib/src/core.c"
contains "$tmp/project-source-dir/build/qstar/compile_commands.json" "app/src/main.c"

cp -R "$project_root/systems-firmware" "$tmp/project-firmware"
"$qstar" --file "$tmp/project-firmware/qstar.lua" check //:kernel > "$tmp/project-firmware-check.out" 2> "$tmp/project-firmware-check.err"
contains "$tmp/project-firmware-check.out" "target-count 1"
contains "$tmp/project-firmware-check.out" "generated-action-count 1"
contains "$tmp/project-firmware-check.out" "stage-count 2"
"$qstar" --file "$tmp/project-firmware/qstar.lua" dry-run //:kernel > "$tmp/project-firmware-kernel-dry.out" 2> "$tmp/project-firmware-kernel-dry.err"
contains "$tmp/project-firmware-kernel-dry.out" "profile_target arch=aarch64 cpu=cortex-a76 abi=lp64 freestanding=true"
contains "$tmp/project-firmware-kernel-dry.out" "-ffreestanding"
contains "$tmp/project-firmware-kernel-dry.out" "-mgeneral-regs-only"
contains "$tmp/project-firmware-kernel-dry.out" "-D__QSTAR_FIRMWARE__=1"
contains "$tmp/project-firmware-kernel-dry.out" "-T"
contains "$tmp/project-firmware-kernel-dry.out" "linker/rpi5-aarch64.ld"
contains "$tmp/project-firmware-kernel-dry.out" "--defsym=__stack_top=0x810000"
"$qstar" --file "$tmp/project-firmware/qstar.lua" build //:kernel --explain-cache > "$tmp/project-firmware-kernel-build.out" 2> "$tmp/project-firmware-kernel-build.err"
contains "$tmp/project-firmware-kernel-build.out" "status ok"
test -f "$tmp/project-firmware/build/qstar/out/___kernel/kernel.elf" || fail "systems firmware kernel artifact missing"
"$qstar" --file "$tmp/project-firmware/qstar.lua" action-log //:kernel:compile:0 > "$tmp/project-firmware-kernel-compile0-log.out" 2> "$tmp/project-firmware-kernel-compile0-log.err"
"$qstar" --file "$tmp/project-firmware/qstar.lua" action-log //:kernel:compile:1 > "$tmp/project-firmware-kernel-compile1-log.out" 2> "$tmp/project-firmware-kernel-compile1-log.err"
"$qstar" --file "$tmp/project-firmware/qstar.lua" action-log //:kernel:link:0 > "$tmp/project-firmware-kernel-link-log.out" 2> "$tmp/project-firmware-kernel-link-log.err"
contains "$tmp/project-firmware-kernel-compile0-log.out" "boot/start.S"
contains "$tmp/project-firmware-kernel-compile1-log.out" "src/kernel.c"
contains "$tmp/project-firmware-kernel-link-log.out" "-T linker/rpi5-aarch64.ld"
contains "$tmp/project-firmware-kernel-link-log.out" "--defsym=__rpi_load_addr=0x80000"
"$qstar" --file "$tmp/project-firmware/qstar.lua" build //:kernel_img --explain-cache --verbose > "$tmp/project-firmware-img-build.out" 2> "$tmp/project-firmware-img-build.err"
contains "$tmp/project-firmware-img-build.out" "build_generated_action //:kernel_img"
contains "$tmp/project-firmware-img-build.out" "output_identity=[generated/kernel8.img|group=images|format=raw-binary|address=0x80000|layout=rpi5-kernel8]"
contains "$tmp/project-firmware-img-build.out" "status ok"
test -f "$tmp/project-firmware/generated/kernel8.img" || fail "systems firmware raw image missing"
contains "$tmp/project-firmware/generated/kernel8.img" "RAW-BINARY"
"$qstar" --file "$tmp/project-firmware/qstar.lua" build //:kernel_img --explain-cache --verbose > "$tmp/project-firmware-img-skip.out" 2> "$tmp/project-firmware-img-skip.err"
contains "$tmp/project-firmware-img-skip.out" "build_action id=//:kernel_img:generate:0 status=skip"
printf "kernel-drift\n" >> "$tmp/project-firmware/build/qstar/out/___kernel/kernel.elf"
"$qstar" --file "$tmp/project-firmware/qstar.lua" build //:kernel_img --explain-cache > "$tmp/project-firmware-img-rebuild.out" 2> "$tmp/project-firmware-img-rebuild.err"
contains "$tmp/project-firmware-img-rebuild.out" "cache_miss id=//:kernel_img:generate:0 reason=input-changed"
"$qstar" --file "$tmp/project-firmware/qstar.lua" stage //:rpi --dry-run > "$tmp/project-firmware-rpi-dry.out" 2> "$tmp/project-firmware-rpi-dry.err"
contains "$tmp/project-firmware-rpi-dry.out" "stage_file src=build/qstar/out/___kernel/kernel.elf dst=stage/rpi/kernel.elf mode=dry-run"
contains "$tmp/project-firmware-rpi-dry.out" "stage_file src=generated/kernel8.img dst=stage/rpi/kernel8.img mode=dry-run"
contains "$tmp/project-firmware-rpi-dry.out" "description=\"Staging //:rpi\""
"$qstar" --file "$tmp/project-firmware/qstar.lua" stage //:rpi > "$tmp/project-firmware-rpi-stage.out" 2> "$tmp/project-firmware-rpi-stage.err"
contains "$tmp/project-firmware-rpi-stage.out" "stage_file src=boot/config.txt dst=stage/rpi/config.txt mode=copy"
contains "$tmp/project-firmware-rpi-stage.out" "stage_file src=build/qstar/out/___kernel/kernel.elf dst=stage/rpi/kernel.elf mode=copy"
contains "$tmp/project-firmware-rpi-stage.out" "stage_file src=generated/kernel8.img dst=stage/rpi/kernel8.img mode=copy"
test -f "$tmp/project-firmware/stage/rpi/config.txt" || fail "systems firmware staged config missing"
test -f "$tmp/project-firmware/stage/rpi/kernel.elf" || fail "systems firmware staged kernel elf missing"
test -f "$tmp/project-firmware/stage/rpi/kernel8.img" || fail "systems firmware staged kernel image missing"
contains "$tmp/project-firmware/build/qstar/stage/___rpi/manifest.json" "\"dst\":\"stage/rpi/kernel8.img\""
"$qstar" --file "$tmp/project-firmware/qstar.lua" build //:qemu_smoke > "$tmp/project-firmware-qemu.out" 2> "$tmp/project-firmware-qemu.err"
contains "$tmp/project-firmware-qemu.out" "run_target label=//:qemu_smoke"
contains "$tmp/project-firmware-qemu.out" "run_marker label=//:qemu_smoke status=matched marker=QSTAR-SMOKE-DONE source=marker_log path=serial.log"
contains "$tmp/project-firmware/serial.log" "QSTAR-SMOKE-DONE"
"$qstar" --file "$tmp/project-firmware/qstar.lua" --profile uefi-x64 dry-run //:uefi_boot > "$tmp/project-firmware-uefi-dry.out" 2> "$tmp/project-firmware-uefi-dry.err"
contains "$tmp/project-firmware-uefi-dry.out" "response_style=msvc"
contains "$tmp/project-firmware-uefi-dry.out" "/out:build/qstar/out/___uefi_boot/BOOTX64.EFI"
contains "$tmp/project-firmware-uefi-dry.out" "/subsystem:efi_application"
"$qstar" --file "$tmp/project-firmware/qstar.lua" --profile uefi-x64 build //:uefi_boot > "$tmp/project-firmware-uefi-build.out" 2> "$tmp/project-firmware-uefi-build.err"
contains "$tmp/project-firmware-uefi-build.out" "status ok"
test -f "$tmp/project-firmware/build/qstar/out/___uefi_boot/BOOTX64.EFI" || fail "systems firmware UEFI artifact missing"
"$qstar" --file "$tmp/project-firmware/qstar.lua" --profile uefi-x64 action-log //:uefi_boot:link:0 > "$tmp/project-firmware-uefi-link-log.out" 2> "$tmp/project-firmware-uefi-link-log.err"
contains "$tmp/project-firmware-uefi-link-log.out" "/entry:efi_main"
"$qstar" --file "$tmp/project-firmware/qstar.lua" --profile uefi-aa64 dry-run //:uefi_boot > "$tmp/project-firmware-uefi-aa64-dry.out" 2> "$tmp/project-firmware-uefi-aa64-dry.err"
contains "$tmp/project-firmware-uefi-aa64-dry.out" "/out:build/qstar/out/___uefi_boot/BOOTAA64.EFI"
"$qstar" --file "$tmp/project-firmware/qstar.lua" --profile uefi-x64 stage //:esp --dry-run > "$tmp/project-firmware-esp-dry.out" 2> "$tmp/project-firmware-esp-dry.err"
contains "$tmp/project-firmware-esp-dry.out" "stage_file src=build/qstar/out/___uefi_boot/BOOTX64.EFI dst=stage/esp/EFI/BOOT/BOOTX64.EFI mode=dry-run"
"$qstar" --file "$tmp/project-firmware/qstar.lua" --profile uefi-x64 stage //:esp > "$tmp/project-firmware-esp-stage.out" 2> "$tmp/project-firmware-esp-stage.err"
contains "$tmp/project-firmware-esp-stage.out" "status ok"
test -f "$tmp/project-firmware/stage/esp/EFI/BOOT/BOOTX64.EFI" || fail "systems firmware ESP BOOTX64.EFI missing"

cp -R "$project_root/systems-firmware" "$tmp/project-firmware-link-fail"
cat > "$tmp/project-firmware-link-fail/tools/fake-link.sh" <<'EOF'
#!/bin/sh
exit 23
EOF
chmod +x "$tmp/project-firmware-link-fail/tools/fake-link.sh"
if "$qstar" --file "$tmp/project-firmware-link-fail/qstar.lua" --diagnostics json build //:kernel > "$tmp/project-firmware-link-fail.out" 2> "$tmp/project-firmware-link-fail.err"; then
	fail "systems firmware link failure unexpectedly succeeded"
fi
contains "$tmp/project-firmware-link-fail.out" "action_diagnostic_json"
contains "$tmp/project-firmware-link-fail.out" "\"failure_kind\":\"link-failure\""
contains "$tmp/project-firmware-link-fail.err" "\"field\":\"link-failure\""
contains "$tmp/project-firmware-link-fail/build/qstar/logs/last-failure.replay" "failure_kind=link-failure"
contains "$tmp/project-firmware-link-fail/build/qstar/logs/last-failure.replay" "tools/fake-link.sh"
"$qstar" --file "$tmp/project-firmware-link-fail/qstar.lua" last-failure > "$tmp/project-firmware-link-last.out" 2> "$tmp/project-firmware-link-last.err"
contains "$tmp/project-firmware-link-last.out" "failure_kind=link-failure"
"$qstar" --file "$tmp/project-firmware-link-fail/qstar.lua" replay //:kernel:link:0 > "$tmp/project-firmware-link-replay.out" 2> "$tmp/project-firmware-link-replay.err"
contains "$tmp/project-firmware-link-replay.out" "tools/fake-link.sh"

cp -R "$project_root/systems-firmware" "$tmp/project-firmware-objcopy-fail"
"$qstar" --file "$tmp/project-firmware-objcopy-fail/qstar.lua" build //:kernel > "$tmp/project-firmware-objcopy-kernel.out" 2> "$tmp/project-firmware-objcopy-kernel.err"
cat > "$tmp/project-firmware-objcopy-fail/tools/fake-objcopy.sh" <<'EOF'
#!/bin/sh
exit 42
EOF
chmod +x "$tmp/project-firmware-objcopy-fail/tools/fake-objcopy.sh"
if "$qstar" --file "$tmp/project-firmware-objcopy-fail/qstar.lua" --diagnostics json build //:kernel_img > "$tmp/project-firmware-objcopy-fail.out" 2> "$tmp/project-firmware-objcopy-fail.err"; then
	fail "systems firmware objcopy failure unexpectedly succeeded"
fi
contains "$tmp/project-firmware-objcopy-fail.out" "action_diagnostic_json"
contains "$tmp/project-firmware-objcopy-fail.out" "\"label\":\"//:kernel_img\""
contains "$tmp/project-firmware-objcopy-fail.out" "\"failure_kind\":\"objcopy-failure\""
contains "$tmp/project-firmware-objcopy-fail.err" "\"field\":\"objcopy-failure\""
contains "$tmp/project-firmware-objcopy-fail/build/qstar/logs/last-failure.replay" "failure_kind=objcopy-failure"
"$qstar" --file "$tmp/project-firmware-objcopy-fail/qstar.lua" replay //:kernel_img:generate:0 > "$tmp/project-firmware-objcopy-replay.out" 2> "$tmp/project-firmware-objcopy-replay.err"
contains "$tmp/project-firmware-objcopy-replay.out" "tools/fake-objcopy.sh"

cp -R "$project_root/systems-firmware" "$tmp/project-firmware-package-fail"
"$qstar" --file "$tmp/project-firmware-package-fail/qstar.lua" build //:kernel > "$tmp/project-firmware-package-kernel.out" 2> "$tmp/project-firmware-package-kernel.err"
"$qstar" --file "$tmp/project-firmware-package-fail/qstar.lua" build //:kernel_img > "$tmp/project-firmware-package-img.out" 2> "$tmp/project-firmware-package-img.err"
printf "not-a-directory\n" > "$tmp/project-firmware-package-fail/stage"
if "$qstar" --file "$tmp/project-firmware-package-fail/qstar.lua" --diagnostics json stage //:rpi > "$tmp/project-firmware-package-fail.out" 2> "$tmp/project-firmware-package-fail.err"; then
	fail "systems firmware package failure unexpectedly succeeded"
fi
contains "$tmp/project-firmware-package-fail.out" "stage_result label=//:rpi status=fail failure_kind=package-failure"
contains "$tmp/project-firmware-package-fail.err" "\"field\":\"package-failure\""
contains "$tmp/project-firmware-package-fail/build/qstar/logs/last-failure.replay" "failure_kind=package-failure"
contains "$tmp/project-firmware-package-fail/build/qstar/logs/last-failure.replay" "qstar stage //:rpi"
"$qstar" --file "$tmp/project-firmware-package-fail/qstar.lua" last-failure > "$tmp/project-firmware-package-last.out" 2> "$tmp/project-firmware-package-last.err"
contains "$tmp/project-firmware-package-last.out" "failure_kind=package-failure"

cp -R "$project_root/systems-firmware" "$tmp/project-firmware-qemu-timeout"
cat > "$tmp/project-firmware-qemu-timeout/tools/qemu-smoke.sh" <<'EOF'
#!/bin/sh
sleep 5
EOF
chmod +x "$tmp/project-firmware-qemu-timeout/tools/qemu-smoke.sh"
if "$qstar" --file "$tmp/project-firmware-qemu-timeout/qstar.lua" --diagnostics json build //:qemu_smoke > "$tmp/project-firmware-qemu-timeout.out" 2> "$tmp/project-firmware-qemu-timeout.err"; then
	fail "systems firmware qemu timeout unexpectedly succeeded"
fi
contains "$tmp/project-firmware-qemu-timeout.out" "run_target_result label=//:qemu_smoke status=timeout timeout_sec=3"
contains "$tmp/project-firmware-qemu-timeout.out" "\"failure_kind\":\"qemu-timeout\""
contains "$tmp/project-firmware-qemu-timeout.err" "\"field\":\"qemu-timeout\""
contains "$tmp/project-firmware-qemu-timeout/build/qstar/logs/last-failure.replay" "failure_kind=qemu-timeout"
"$qstar" --file "$tmp/project-firmware-qemu-timeout/qstar.lua" last-failure > "$tmp/project-firmware-qemu-last.out" 2> "$tmp/project-firmware-qemu-last.err"
contains "$tmp/project-firmware-qemu-last.out" "failure_kind=qemu-timeout"

cp -R "$project_root/systems-firmware" "$tmp/project-firmware-cache-tool"
"$qstar" --file "$tmp/project-firmware-cache-tool/qstar.lua" build //:kernel > "$tmp/project-firmware-cache-tool-kernel.out" 2> "$tmp/project-firmware-cache-tool-kernel.err"
"$qstar" --file "$tmp/project-firmware-cache-tool/qstar.lua" build //:kernel_img > "$tmp/project-firmware-cache-tool-first.out" 2> "$tmp/project-firmware-cache-tool-first.err"
cp "$tmp/project-firmware-cache-tool/tools/fake-objcopy.sh" "$tmp/project-firmware-cache-tool/tools/fake-objcopy-v2.sh"
chmod +x "$tmp/project-firmware-cache-tool/tools/fake-objcopy-v2.sh"
awk '{ gsub("llvm-objcopy=tools/fake-objcopy.sh", "llvm-objcopy=tools/fake-objcopy-v2.sh"); print }' "$tmp/project-firmware-cache-tool/qstar.lua" > "$tmp/project-firmware-cache-tool/qstar.lua.new"
mv "$tmp/project-firmware-cache-tool/qstar.lua.new" "$tmp/project-firmware-cache-tool/qstar.lua"
"$qstar" --file "$tmp/project-firmware-cache-tool/qstar.lua" build //:kernel_img --explain-cache > "$tmp/project-firmware-cache-tool-second.out" 2> "$tmp/project-firmware-cache-tool-second.err"
contains "$tmp/project-firmware-cache-tool-second.out" "cache_miss id=//:kernel_img:generate:0 reason=external-tool-changed"

cp -R "$project_root/systems-firmware" "$tmp/project-firmware-cache-output"
"$qstar" --file "$tmp/project-firmware-cache-output/qstar.lua" build //:kernel > "$tmp/project-firmware-cache-output-kernel.out" 2> "$tmp/project-firmware-cache-output-kernel.err"
"$qstar" --file "$tmp/project-firmware-cache-output/qstar.lua" build //:kernel_img > "$tmp/project-firmware-cache-output-first.out" 2> "$tmp/project-firmware-cache-output-first.err"
awk '{ gsub("generated/kernel8.img", "generated/kernel9.img"); print }' "$tmp/project-firmware-cache-output/qstar.lua" > "$tmp/project-firmware-cache-output/qstar.lua.new"
mv "$tmp/project-firmware-cache-output/qstar.lua.new" "$tmp/project-firmware-cache-output/qstar.lua"
"$qstar" --file "$tmp/project-firmware-cache-output/qstar.lua" build //:kernel_img --explain-cache > "$tmp/project-firmware-cache-output-second.out" 2> "$tmp/project-firmware-cache-output-second.err"
contains "$tmp/project-firmware-cache-output-second.out" "cache_miss id=//:kernel_img:generate:0 reason=output-changed"

contains "docs/qstar-v0-seal.md" "qstar/tests/manual/c-only"
contains "docs/qstar-v0-seal.md" "qstar/tests/manual/generated"
contains "docs/qstar-v0-seal.md" "qstar/tests/manual/mixed-cale"
contains "docs/qstar-v0-seal.md" "make -C qstar qstar-v0-release-tests"
contains "docs/qstar-v0-seal.md" "qstar-v0.1-release-tests"
contains "docs/qstar-v0-seal.md" "qstar-v0.2-release-candidate-seal.md"
contains "docs/qstar-v0.1-hardening-seal.md" "status: v0.1 standalone developer build system"
contains "docs/qstar-v0.1-hardening-seal.md" "make -C qstar qstar-v0.1-release-tests"
contains "docs/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/c-app-lib-test"
contains "docs/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/cxx-mixed"
contains "docs/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/generated-config"
contains "docs/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/multipkg"
contains "docs/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/systems-firmware"
contains "docs/qstar-v0.1-hardening-seal.md" "Cale build integration: deferred"
contains "docs/qstar-v0.1-hardening-seal.md" "qstar action-log <action-id>"
contains "docs/qstar-v0.1-hardening-seal.md" "qstar replay <action-id>"
contains "README.md" "QStar is a standalone build system"
contains "README.md" "v0.6.1-beta"
contains "README.md" "English | [한국어](README.ko.md)"
contains "README.md" "make qstar-self-host-tests"
contains "README.md" "make qstar-public-beta-download-smoke"
contains "README.md" "Stella/Ninja medium performance artifact collection"
contains "README.md" "Linux x86_64 runtime tarballs"
contains "README.md" "Manual native validation candidate"
contains "README.md" "GitHub Wiki"
contains "README.md" "Apache License, Version 2.0"
contains "README.ko.md" "[English](README.md) | 한국어"
contains "README.ko.md" "v0.6.1-beta"
contains "README.ko.md" "make qstar-public-beta-download-smoke"
contains "README.ko.md" "Linux x86_64 runtime tarball"
contains "README.ko.md" "Stella/Ninja medium performance"
contains "README.ko.md" "manual native validation candidate"
contains "docs/qstar-v0.2-release-candidate-seal.md" "status: v0.2 release candidate"
contains "docs/qstar-v0.2-release-candidate-seal.md" "qstar-v0.2-rc-tests"
contains "docs/qstar-v0.2-release-candidate-seal.md" "qstar-systems-corpus-tests"
contains "docs/qstar-v0.3-seal.md" "status: v0.3 release candidate"
contains "docs/qstar-v0.3-seal.md" "qstar-v0.3-rc-tests"
contains "docs/qstar-v0.3-seal.md" "qstar --version"
contains "docs/qstar-v0.3-seal.md" "stable surface"
contains "docs/qstar-v0.3-seal.md" "experimental surface"
contains "docs/qstar-v0.4-stella-seal.md" "status: v0.4 Stella workflow seal"
contains "docs/qstar-v0.4-stella-seal.md" "runtime version: qstar 0.4.0-beta.1"
contains "docs/qstar-v0.4-stella-seal.md" "extension package: qstar-vscode 0.3.0"
contains "docs/qstar-v0.4-stella-seal.md" "qstar-v0.4-pilot-tests"
contains "docs/qstar-v0.4-stella-seal.md" "make install"
contains "docs/qstar-v0.4-stella-seal.md" "qstar-linux-validation-tests"
contains "docs/qstar-v0.4-stella-seal.md" "qstar-windows-prep-tests"
contains "docs/qstar-v0.4-stella-seal.md" "qstar-public-beta-release-tests"
contains "docs/qstar-v0.4-stella-seal.md" "code --install-extension dist/qstar-vscode-0.3.0.vsix --force"
contains "docs/qstar-v0.6-readiness.md" "status: 0.6 beta readiness gate"
contains "docs/qstar-v0.6-readiness.md" "qstar 0.6.1-beta"
contains "docs/qstar-v0.6-readiness.md" "Stella daemon"
contains "docs/qstar-v0.6-readiness.md" "make qstar-public-beta-release-tests"
contains "docs/qstar-v0.6-readiness.md" "qstar-v0.6.1-beta-macos-arm64.tar.gz"
contains "docs/qstar-v0.6-readiness.md" "qstar-v0.6.1-beta-linux-x86_64.tar.gz"
contains "docs/qstar-v0.6-readiness.md" "Linux"
contains "docs/qstar-v0.6-readiness.md" "Windows"
contains "docs/qstar-v0.6-post-release-smoke.md" "status: post-release download smoke required for release seal"
contains "docs/qstar-v0.6-post-release-smoke.md" "make qstar-public-beta-download-smoke"
contains "docs/qstar-v0.6-post-release-smoke.md" "tools/smoke-github-release.sh"
contains "docs/qstar-v0.6-post-release-smoke.md" "qstar-v0.6.1-beta-macos-arm64.tar.gz"
contains "docs/qstar-v0.6-post-release-smoke.md" "qstar-v0.6.1-beta-linux-x86_64.tar.gz"
contains "docs/qstar-v0.6-post-release-smoke.md" "SHA256SUMS"
contains "docs/daemon-beta-readiness.md" "documented beta opt-in feature"
contains "docs/daemon-beta-readiness.md" "default"
contains "docs/daemon-beta-readiness.md" "Windows named pipe"
contains "docs/daemon-beta-readiness.md" "0.6.0-beta"
contains "docs/daemon-beta-readiness.md" "medium_project_gate backend=stella-daemon phase=clean"
contains "docs/performance-gates.md" "Round Q137 local macOS arm64"
contains "docs/performance-gates.md" "medium_project_gate backend=stella-jobs"
contains "docs/performance-gates.md" "backend=stella-daemon"
contains "docs/performance-gates.md" "socket-bind-not-permitted"
contains "docs/performance-gates.md" "Linux CI Performance Artifacts"
contains "docs/performance-gates.md" "medium_project_gate scheduler runner=posix_spawn event_wait=poll"
contains "docs/performance-gates.md" "dist/perf/linux-<compiler>-medium-perf.txt"
contains "docs/performance-gates.md" "async_final_actions"
contains "docs/performance-gates.md" "Large Synthetic Corpus Gate"
contains "docs/performance-gates.md" "large_project_gate"
contains "docs/performance-gates.md" 'POSIX `poll()`'
contains "wiki/reference/performance-gates.md" "Round Q137 local macOS arm64"
contains "wiki/reference/performance-gates.md" "medium_project_gate backend=stella-jobs"
contains "wiki/reference/performance-gates.md" "backend=stella-daemon"
contains "wiki/reference/performance-gates.md" "socket-bind-not-permitted"
contains "wiki/reference/performance-gates.md" "Linux CI Performance Artifacts"
contains "wiki/reference/performance-gates.md" "medium_project_gate scheduler runner=posix_spawn event_wait=poll"
contains "wiki/reference/performance-gates.md" "dist/perf/linux-<compiler>-medium-perf.txt"
contains "wiki/reference/performance-gates.md" "async_final_actions"
contains "wiki/reference/performance-gates.md" "Large Synthetic Corpus"
contains "wiki/reference/performance-gates.md" "large_project_gate"
contains "wiki/reference/performance-gates.md" "POSIX"
contains "docs/perf/stella-plan-cache-design.md" "Q129 POSIX Spawn Runner MVP"
contains "docs/perf/stella-plan-cache-design.md" "Q130 Event-Driven Output Drain"
contains "docs/perf/stella-plan-cache-design.md" "Q131 Lazy Success Action Logs"
contains "docs/perf/stella-plan-cache-design.md" "Q137 Stella/Ninja Performance Seal"
contains "docs/perf/stella-plan-cache-design.md" "persistent Stella daemon design"
contains "docs/daemon/stella-daemon.md" "command namespace: qstar daemon"
contains "docs/daemon/stella-daemon.md" "qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --start"
contains "docs/daemon/stella-daemon.md" "qstar build //:app --use-daemon=auto"
contains "docs/daemon/stella-daemon.md" "Unix domain socket"
contains "docs/daemon/stella-daemon.md" "Windows named pipe"
contains "docs/daemon/stella-daemon.md" "workspace.info"
contains "docs/daemon/stella-daemon.md" "qstar daemon --socket path --query method"
contains "docs/contracts/daemon-read-api.md" "qstar-daemon-query-v1"
contains "docs/contracts/daemon-read-api.md" "compile_commands.path"
contains "docs/contracts/daemon-read-api.md" "build.summary"
contains "docs/contracts/daemon-read-api.md" "build/test/clean 같은 mutation"
contains "docs/daemon/stella-daemon.md" "Existing socket cleanup is conservative"
contains "docs/daemon-beta-readiness.md" "Q153 이후 owner-only socket directory/file"
contains "docs/perf/stella-ninja-profile.md" "runner=posix_spawn|fork"
contains "docs/perf/stella-ninja-profile.md" "Q130: Event-Driven Output Drain"
contains "docs/perf/stella-ninja-profile.md" "Q133-Q137: Scheduler Semantics And Performance Seal"
contains "docs/perf/stella-ninja-profile.md" "persistent Stella daemon"
contains "docs/progress-output.md" "status: progress output contract"
contains "docs/progress-output.md" "[ 75%] Linking CXX executable app"
contains "docs/progress-output.md" "Stella progress renderer and warning/error stream coloring active"
contains "docs/progress-output.md" "legacy scheduler stage wording"
contains "docs/progress-output.md" "action_description"
contains "docs/progress-output.md" "action-log, replay, last-failure preserve descriptions"
contains "docs/progress-output.md" "successful Stella"
contains "docs/progress-output.md" "description='Building C object"
contains "docs/progress-output.md" "qstar.status"
contains "docs/progress-output.md" "warning:"
contains "docs/progress-output.md" "--progress auto"
contains "docs/progress-output.md" "--schedule-trace"
not_contains "docs/progress-output.md" "[5%] Stage"
not_contains "docs/progress-output.md" "[ 5%] Stage"
not_contains "docs/progress-output.md" "Stage 1:"
contains "docs/linux-validation.md" "Linux Validation Path"
contains "docs/linux-validation.md" "make install PREFIX=/tmp/qstar-linux-smoke"
contains "docs/linux-validation.md" ".github/workflows/linux-validation.yml"
contains "docs/linux-validation.md" "QSTAR_LINUX_VALIDATION_CC=clang"
contains "docs/linux-validation.md" "ubuntu-latest / gcc"
contains "docs/linux-validation.md" "CC=clang make qstar-linux-validation-tests"
contains "docs/linux-validation.md" "medium_project_gate scheduler runner=posix_spawn event_wait=poll"
contains "docs/linux-validation.md" "dist/perf/linux-<compiler>-medium-perf.txt"
contains "docs/linux-validation.md" "workflow_dispatch / large-performance-report"
contains "docs/linux-validation.md" "workflow_dispatch / daemon_socket_smoke=true"
contains "docs/linux-validation.md" "QSTAR_RELEASE_PLATFORM=linux-x86_64"
contains "docs/linux-validation.md" "file-linux-x86_64.txt"
contains "docs/linux-validation.md" "ldd-linux-x86_64.txt"
contains "docs/linux-validation.md" "extract-docs-show-qstar-lua.txt"
contains "docs/linux-validation.md" "qstar-linux-x86_64-release-candidate-dry-run"
contains ".github/workflows/linux-validation.yml" "ubuntu-latest"
contains ".github/workflows/linux-validation.yml" "submodules: recursive"
contains ".github/workflows/linux-validation.yml" "ninja-build"
contains ".github/workflows/linux-validation.yml" "make check"
contains ".github/workflows/linux-validation.yml" "make qstar-linux-validation-tests"
contains ".github/workflows/linux-validation.yml" "Run explicit Ninja backend parity gate"
contains ".github/workflows/linux-validation.yml" "QSTAR_LINUX_VALIDATION_CC"
contains ".github/workflows/linux-validation.yml" "docs --path"
contains ".github/workflows/linux-validation.yml" "Medium Stella/Ninja performance line protocol"
contains ".github/workflows/linux-validation.yml" "medium_project_gate scheduler runner=posix_spawn event_wait=poll"
contains ".github/workflows/linux-validation.yml" "actions/upload-artifact@v4"
contains ".github/workflows/linux-validation.yml" "large-performance-report"
contains ".github/workflows/linux-validation.yml" "daemon_socket_smoke"
contains ".github/workflows/linux-validation.yml" "daemon-socket-smoke"
contains ".github/workflows/linux-validation.yml" "publish_linux_asset"
contains ".github/workflows/linux-validation.yml" "publish-linux-release-asset"
contains ".github/workflows/linux-validation.yml" "QSTAR_RELEASE_PLATFORM=linux-x86_64"
contains ".github/workflows/linux-validation.yml" "Linux release tarball packaging dry-run"
contains ".github/workflows/linux-validation.yml" "Upload Linux release-candidate dry-run artifact"
contains ".github/workflows/linux-validation.yml" "file-linux-x86_64.txt"
contains ".github/workflows/linux-validation.yml" "ldd-linux-x86_64.txt"
contains ".github/workflows/linux-validation.yml" "extract-docs-show-qstar-lua.txt"
contains ".github/workflows/linux-validation.yml" "qstar-linux-x86_64-release-candidate-dry-run"
contains ".github/workflows/linux-validation.yml" "qstar-linux-daemon-medium-performance"
contains ".github/workflows/linux-validation.yml" "qstar-linux-x86_64-published-release-asset"
contains "tests/linux-validation.sh" "QSTAR_LINUX_VALIDATION_CC"
contains "tests/medium-project-performance.sh" "event_wait=poll"
contains "tests/medium-project-performance.sh" "medium_project_gate scheduler runner=%s event_wait=%s"
contains "README.md" "git clone --recurse-submodules"
contains "README.md" "Stella daemon beta opt-in workflow"
contains "README.md" "extracted tarball smoke"
contains "README.ko.md" "git clone --recurse-submodules"
contains "README.ko.md" "Stella daemon beta opt-in workflow"
contains "README.ko.md" "extracted tarball smoke"
contains "wiki/installation.md" "git submodule update --init --recursive"
contains "wiki/installation.md" "QSTAR_RELEASE_PLATFORM=linux-x86_64"
contains "wiki/installation.md" "qstar-public-beta-download-smoke"
contains "wiki/installation.md" "daemon_socket_smoke=true"
contains "wiki/installation.md" "publish_linux_asset=true"
contains "docs/windows-path-process.md" "Windows Path And Process Prep"
contains "docs/windows-path-process.md" "qstar-windows-prep-tests"
contains "docs/windows-path-process.md" "sources = {\"src\\\\main.c\"}       -- invalid"
contains "docs/windows-path-process.md" ".github/workflows/windows-validation.yml"
contains "docs/windows-path-process.md" "workflow_dispatch"
contains "docs/windows-path-process.md" "MSVC response-file escaping"
contains "docs/windows-path-process.md" "drive-letter paths are not allowed"
contains ".github/workflows/windows-validation.yml" "workflow_dispatch"
contains ".github/workflows/windows-validation.yml" "msys2/setup-msys2"
contains ".github/workflows/windows-validation.yml" "make qstar-windows-prep-tests"
contains "tests/windows-prep.sh" "windows-msvc-fake"
contains "tests/windows-prep.sh" "___windows_rsp_compile_0.rsp"
contains "tests/windows-prep.sh" "/DTRAIL=tail"
contains "tests/corpus/response-files/qstar.lua" "msvc_response_escape_args"
contains "tests/corpus/response-files/tools/fake-clang-cl" "fake-clang-cl: output path not found"
contains "docs/qstar-pilot-readiness-seal.md" "status: pilot-readiness seal"
contains "docs/qstar-pilot-readiness-seal.md" "qstar-pilot-readiness-tests"
contains "docs/qstar-pilot-readiness-seal.md" "qstar <subcommand> --help"
contains "docs/README.md" "../wiki/AI_INDEX.md"
contains "man/man1/qstar.1" "QSTAR_DOC_DIR"
contains "man/man5/qstar-lua.5" "QSTAR_VERSION"
contains "Makefile" "share/doc/qstar"
contains "Makefile" "man/man1/qstar.1"
contains "Makefile" "man/man5/qstar-lua.5"
contains "docs/qstar-submodule-extraction-prep.md" "standalone-repository-extracted"
contains "docs/qstar-submodule-extraction-prep.md" "actual split: completed"
contains "docs/qstar-submodule-extraction-prep.md" "vendor/lua"
contains "LICENSE.md" "Apache License"
contains "LICENSE/README.md" "QStar itself is distributed under the Apache License"
contains "LICENSE/lua.txt" "Copyright (C) 1994-2025 Lua.org"
contains "docs/README.md" "qstar-v0.2-release-candidate-seal.md"
contains "docs/README.md" "qstar-v0.3-seal.md"
contains "docs/README.md" "qstar-v0.4-stella-seal.md"
contains "docs/README.md" "linux-validation.md"
contains "docs/README.md" "daemon-beta-readiness.md"
contains "docs/README.md" "daemon/stella-daemon.md"
contains "docs/README.md" "contracts/daemon-read-api.md"
contains "docs/README.md" "windows-path-process.md"
contains "docs/README.md" "manual native validation candidate workflow"
contains "docs/README.md" "public-beta-release.md"
contains "docs/README.md" "qstar-v0.6-post-release-smoke.md"
contains "docs/README.md" "releases/TEMPLATE.md"
contains "docs/README.md" "progress-output.md"
contains "docs/README.md" "qstar-v0.6-readiness.md"
contains "docs/README.md" "qstar-v0.5-readiness.md"
contains "docs/README.md" "qstar-pilot-readiness-seal.md"
contains "docs/README.md" "qstar-submodule-extraction-prep.md"
contains "docs/public-beta-release.md" "Public Beta Release Gate"
contains "docs/public-beta-release.md" "qstar-v0.6.1-beta-macos-arm64.tar.gz"
contains "docs/public-beta-release.md" "qstar-v0.6.1-beta-linux-x86_64.tar.gz"
contains "docs/public-beta-release.md" "0.6.1-beta"
contains "docs/public-beta-release.md" "qstar-public-beta-release-tests"
contains "docs/public-beta-release.md" "qstar-public-beta-download-smoke"
contains "docs/public-beta-release.md" "tools/smoke-github-release.sh"
contains "docs/public-beta-release.md" "tools/sync-github-wiki.sh"
contains "docs/public-beta-release.md" "VSCode extension is not included"
contains "docs/public-beta-release.md" "qstar-linux-x86_64-release-candidate-dry-run"
contains "docs/public-beta-release.md" "daemon_socket_smoke=true"
contains "docs/public-beta-release.md" "publish_linux_asset=true"
contains "docs/releases/TEMPLATE.md" "Release Asset Checklist"
contains "docs/releases/TEMPLATE.md" "Ubuntu gcc/clang CI"
contains "docs/releases/TEMPLATE.md" "Linux medium performance artifacts"
contains "docs/releases/TEMPLATE.md" "extracted tarball"
contains "docs/releases/TEMPLATE.md" ".github/workflows/linux-validation.yml"
contains "docs/releases/v0.6.1-beta.md" "qstar-public-beta-release-tests"
contains "docs/releases/v0.6.1-beta.md" "qstar-v0.6.1-beta-linux-x86_64.tar.gz"
contains "docs/releases/v0.6.1-beta.md" "publish_linux_asset=true"
contains "docs/releases/v0.6.1-beta.md" "qstar 0.6.1-beta"
contains "docs/releases/v0.4.0-beta.1.md" "qstar-public-beta-release-tests"
contains "docs/releases/v0.5.0-beta.1.md" "qstar-public-beta-release-tests"
contains "docs/releases/v0.5.0-beta.1.md" "qstar 0.5.0-beta.1"
contains "docs/releases/v0.5.0-beta.1.md" "manual"
contains "docs/releases/v0.5.0-beta.1.md" "validation candidate"
contains "docs/releases/v0.6.0-beta.md" "qstar-public-beta-release-tests"
contains "docs/releases/v0.6.0-beta.md" "qstar-public-beta-download-smoke"
contains "docs/releases/v0.6.0-beta.md" "qstar 0.6.0-beta"
contains "docs/releases/v0.6.0-beta.md" "sharedlib"
contains "docs/releases/v0.6.0-beta.md" "Cale backend"
contains "docs/releases/v0.6.0-beta.md" "Stella/Ninja medium performance artifacts"
contains "docs/releases/v0.6.0-beta.md" "runner=posix_spawn event_wait=poll"
contains "docs/releases/v0.6.0-beta.md" "backend=stella-daemon"
contains "docs/releases/v0.6.0-beta.md" "daemon_socket_smoke=true"
contains "docs/releases/v0.6.0-beta.md" "socket-bind-not-permitted"
contains "docs/releases/v0.6.0-beta.md" "documented beta opt-in"
contains "docs/releases/v0.6.0-beta.md" "qstar daemon --query"
contains "docs/releases/v0.6.0-beta.md" "Windows named pipe support is deferred"
contains "docs/public-beta-release.md" ".github/workflows/linux-validation.yml"
contains "README.md" "qstar docs --ai-index"
contains "wiki/README.md" "AI_INDEX.md"
contains "wiki/README.md" "reference/progress-output.md"
contains "wiki/README.md" "reference/stella-daemon.md"
contains "wiki/reference/backends.md" "Persistent Stella daemon"
contains "wiki/reference/backends.md" "qstar build --use-daemon=auto|never|always"
contains "wiki/AI_INDEX.md" "QStar AI Index"
contains "wiki/AI_INDEX.md" "qstar.custom_target"
contains "wiki/AI_INDEX.md" "qstar.output(path, {format = \"object\"})"
contains "wiki/AI_INDEX.md" "object artifact bridge"
contains "wiki/AI_INDEX.md" "qstar.import_module"
contains "wiki/AI_INDEX.md" "qstar.config"
contains "wiki/AI_INDEX.md" "qstar.status"
contains "wiki/AI_INDEX.md" "description="
contains "wiki/AI_INDEX.md" "generated_dir"
contains "wiki/AI_INDEX.md" "qstar-public-beta-release-tests"
contains "wiki/AI_INDEX.md" "qstar-public-beta-download-smoke"
contains "wiki/AI_INDEX.md" "qstar-v0.6-post-release-smoke.md"
contains "wiki/AI_INDEX.md" "Progress output contract"
contains "wiki/AI_INDEX.md" "qstar-v0.6-readiness.md"
contains "wiki/AI_INDEX.md" "qstar-v0.5-readiness.md"
contains "wiki/AI_INDEX.md" "qstar-linux-validation-tests"
contains "wiki/AI_INDEX.md" "qstar daemon"
contains "wiki/AI_INDEX.md" "qstar daemon --socket path --start|--stop|--serve|--status"
contains "wiki/AI_INDEX.md" "compile_commands.path"
contains "wiki/AI_INDEX.md" "docs/contracts/daemon-read-api.md"
contains "wiki/AI_INDEX.md" "docs/daemon-beta-readiness.md"
contains "wiki/AI_INDEX.md" "backend=stella-daemon"
contains "wiki/AI_INDEX.md" "socket-bind-not-permitted"
contains "wiki/AI_INDEX.md" "docs/daemon/stella-daemon.md"
contains "wiki/AI_INDEX.md" "Linux validation workflow"
contains "wiki/AI_INDEX.md" "QSTAR_LINUX_VALIDATION_CC"
contains "wiki/AI_INDEX.md" "QSTAR_RELEASE_PLATFORM=linux-x86_64"
contains "wiki/AI_INDEX.md" "qstar-linux-x86_64-release-candidate-dry-run"
contains "wiki/AI_INDEX.md" "publish_linux_asset=true"
contains "wiki/AI_INDEX.md" "daemon_socket_smoke=true"
contains "wiki/AI_INDEX.md" "qstar-windows-prep-tests"
contains "wiki/AI_INDEX.md" ".github/workflows/windows-validation.yml"
contains "wiki/AI_INDEX.md" 'static `.lib`, `.dll`, import library'
contains "wiki/AI_INDEX.md" "qstar --file qstar.lua action-log"
contains "wiki/AI_INDEX.md" "low-level/bootloader-style project"
contains "wiki/reference/progress-output.md" "[ 75%] Linking CXX executable app"
contains "wiki/reference/progress-output.md" "legacy scheduler stage wording"
contains "wiki/reference/progress-output.md" "action_description"
contains "wiki/reference/progress-output.md" "description='Building C object"
contains "wiki/reference/progress-output.md" "qstar.status"
contains "wiki/reference/progress-output.md" "warning:"
not_contains "wiki/reference/progress-output.md" "[5%] Stage"
not_contains "wiki/reference/progress-output.md" "[ 5%] Stage"
not_contains "wiki/reference/progress-output.md" "Stage 1:"
contains "wiki/reference/stella-daemon.md" "qstar daemon --socket"
contains "wiki/reference/stella-daemon.md" "--query compile_commands.path"
contains "wiki/reference/stella-daemon.md" "qstar-daemon-read-v1"
contains "wiki/reference/stella-daemon.md" "documented beta opt-in"
contains "wiki/reference/stella-daemon.md" "0.6.0-beta"
contains "wiki/reference/stella-daemon.md" "qstar build //:app --use-daemon=auto"
contains "wiki/reference/stella-daemon.md" "Unix domain socket"
contains "wiki/reference/stella-daemon.md" "named pipe deferred"
contains "wiki/reference/stella-daemon.md" "Package root mismatch"
contains "man/man1/qstar.1" "Ic action-log"
contains "man/man1/qstar.1" "Fl G Ar stella|ninja|auto"
contains "man/man1/qstar.1" "CMake-style action descriptions"
contains "man/man1/qstar.1" "[ 75%] Linking CXX executable app"
contains "man/man1/qstar.1" "compile_commands.path"
contains "man/man1/qstar.1" "beta opt-in"
contains "man/man1/qstar.1" "warning:"
contains "man/man1/qstar.1" "description="
contains "man/man5/qstar-lua.5" "Ic qstar.group"
contains "man/man5/qstar-lua.5" "Ic qstar.import_module"
contains "man/man5/qstar-lua.5" "Ic qstar.config"
contains "man/man5/qstar-lua.5" "Ic qstar.status"
contains "man/man5/qstar-lua.5" "CMake-style"
contains "man/man5/qstar-lua.5" "Ic qstar.target_file"
contains "man/man5/qstar-lua.5" "object artifact"
contains "man/man5/qstar-lua.5" "Objective-C"
contains "editors/vscode/qstar/snippets/qstar.json" "\"prefix\": \"qdesc\""
contains "editors/vscode/qstar/snippets/qstar.json" "CMake-style progress and replay logs"
contains "Makefile" "qstar-v0.2-rc-tests"
contains "Makefile" "qstar-v0.3-rc-tests"
contains "Makefile" "qstar-pilot-readiness-tests"
contains "Makefile" "qstar-wiki-cli-sync-tests"
contains "Makefile" "qstar-public-beta-release-tests"
contains "Makefile" "qstar-public-beta-linux-package"
contains "Makefile" "qstar-public-beta-github-upload"
contains "Makefile" "qstar-public-beta-download-smoke"
contains "Makefile" "qstar-v0.5-readiness-tests"
contains "Makefile" "qstar-linux-validation-tests"
contains "Makefile" "qstar-windows-prep-tests"
contains "Makefile" "qstar-release-candidate-tests"
contains "Makefile" "qstar-full-regression-tests"
contains "Makefile" "qstar-systems-corpus-tests"
contains "Makefile" "qstar-medium-project-readiness-tests"
contains "Makefile" "qstar-large-project-performance-tests"
contains "Makefile" "qstar-perf-summary-tests"
contains "tools/package-public-beta.sh" "SHA256SUMS"
contains "tools/package-public-beta.sh" "VSCode VSIX must not be included"
contains "tools/package-public-beta.sh" "linux-x86_64 release package must be built on a Linux host"
contains "tools/package-public-beta.sh" "ldd is required for linux-x86_64 release sanity"
contains "tools/package-public-beta.sh" "docs --show reference/qstar-lua.md"
contains "tools/package-public-beta.sh" "extract_smoke="
contains "tools/package-public-beta.sh" "extracted qstar version"
contains "tools/publish-github-release-asset.sh" "qstar-release-upload"
contains "tools/publish-github-release-asset.sh" "gh release upload"
contains "tools/publish-github-release-asset.sh" "SHA256SUMS.merged"
contains "tools/smoke-github-release.sh" "qstar-release-download-smoke"
contains "tools/smoke-github-release.sh" "SHA256SUMS"
contains "tools/smoke-github-release.sh" "docs --show reference/qstar-lua.md"
contains "tools/smoke-github-release.sh" "codesign --verify"
contains "tools/smoke-github-release.sh" "linux-x86_64 download smoke must be run on a Linux host"
contains "tools/sync-github-wiki.sh" "Progress Output"
contains "tools/sync-github-wiki.sh" "Stella Daemon"
contains "tools/perf-summary.sh" "medium_project_gate"
contains "docs/performance-gates.md" "tools/perf-summary.sh --repeat 3 --"
contains "docs/performance-gates.md" "perf_summary ratio"
contains "wiki/reference/performance-gates.md" "tools/perf-summary.sh --repeat 3 --"
contains "wiki/AI_INDEX.md" "tools/perf-summary.sh"

step "perf summary tool" "perf-summary"
cat > "$tmp/perf-summary.in" <<'EOF'
medium_project_gate backend=stella phase=clean elapsed_ms=150
medium_project_gate backend=stella phase=noop elapsed_ms=70
medium_project_gate backend=stella phase=incremental elapsed_ms=95
medium_project_gate backend=stella-daemon phase=clean elapsed_ms=90
medium_project_gate backend=stella-daemon phase=noop elapsed_ms=35
medium_project_gate backend=stella-daemon phase=incremental elapsed_ms=55
medium_project_gate backend=stella-jobs jobs=10 phase=clean elapsed_ms=140
medium_project_gate backend=stella-jobs jobs=10 phase=noop elapsed_ms=68
medium_project_gate backend=stella-jobs jobs=10 phase=incremental elapsed_ms=90
medium_project_gate backend=ninja phase=clean elapsed_ms=100
medium_project_gate backend=ninja phase=noop elapsed_ms=75
medium_project_gate backend=ninja phase=incremental elapsed_ms=120
medium_project_gate backend=stella phase=clean elapsed_ms=170
medium_project_gate backend=stella phase=noop elapsed_ms=72
medium_project_gate backend=stella phase=incremental elapsed_ms=100
medium_project_gate backend=stella-daemon phase=clean elapsed_ms=95
medium_project_gate backend=stella-daemon phase=noop elapsed_ms=38
medium_project_gate backend=stella-daemon phase=incremental elapsed_ms=58
medium_project_gate backend=stella-jobs jobs=10 phase=clean elapsed_ms=145
medium_project_gate backend=stella-jobs jobs=10 phase=noop elapsed_ms=69
medium_project_gate backend=stella-jobs jobs=10 phase=incremental elapsed_ms=92
medium_project_gate backend=ninja phase=clean elapsed_ms=110
medium_project_gate backend=ninja phase=noop elapsed_ms=78
medium_project_gate backend=ninja phase=incremental elapsed_ms=125
medium_project_gate backend=stella phase=clean elapsed_ms=130
medium_project_gate backend=stella phase=noop elapsed_ms=65
medium_project_gate backend=stella phase=incremental elapsed_ms=91
medium_project_gate backend=stella-daemon phase=clean elapsed_ms=85
medium_project_gate backend=stella-daemon phase=noop elapsed_ms=33
medium_project_gate backend=stella-daemon phase=incremental elapsed_ms=52
medium_project_gate backend=stella-jobs jobs=10 phase=clean elapsed_ms=138
medium_project_gate backend=stella-jobs jobs=10 phase=noop elapsed_ms=67
medium_project_gate backend=stella-jobs jobs=10 phase=incremental elapsed_ms=88
medium_project_gate backend=ninja phase=clean elapsed_ms=105
medium_project_gate backend=ninja phase=noop elapsed_ms=77
medium_project_gate backend=ninja phase=incremental elapsed_ms=122
large_project_gate mode=200 backend=stella phase=clean elapsed_ms=900
large_project_gate mode=200 backend=stella phase=clean elapsed_ms=880
large_project_gate mode=200 backend=stella phase=clean elapsed_ms=920
large_project_gate mode=200 backend=stella-daemon phase=clean elapsed_ms=700
large_project_gate mode=200 backend=stella-daemon phase=clean elapsed_ms=690
large_project_gate mode=200 backend=stella-daemon phase=clean elapsed_ms=710
large_project_gate mode=200 backend=ninja phase=clean elapsed_ms=1000
large_project_gate mode=200 backend=ninja phase=clean elapsed_ms=990
large_project_gate mode=200 backend=ninja phase=clean elapsed_ms=1010
EOF
tools/perf-summary.sh "$tmp/perf-summary.in" > "$tmp/perf-summary.out" 2> "$tmp/perf-summary.err"
contains "$tmp/perf-summary.out" "perf_summary sample gate=medium mode=medium backend=stella phase=clean count=3 min_ms=130 median_ms=150 max_ms=170"
contains "$tmp/perf-summary.out" "perf_summary sample gate=medium mode=medium backend=stella-daemon phase=clean count=3 min_ms=85 median_ms=90 max_ms=95"
contains "$tmp/perf-summary.out" "perf_summary sample gate=large mode=200 backend=stella phase=clean count=3 min_ms=880 median_ms=900 max_ms=920"
contains "$tmp/perf-summary.out" "perf_summary sample gate=large mode=200 backend=stella-daemon phase=clean count=3 min_ms=690 median_ms=700 max_ms=710"
contains "$tmp/perf-summary.out" "perf_summary ratio gate=medium mode=medium backend=stella phase=clean backend_median_ms=150 ninja_median_ms=105 ratio_x100=143"
contains "$tmp/perf-summary.out" "perf_summary ratio gate=medium mode=medium backend=stella-daemon phase=clean backend_median_ms=90 ninja_median_ms=105 ratio_x100=86"
contains "$tmp/perf-summary.out" "perf_summary status=ok sample_count=15 ratio_count=11 warning_count=0 hard=0"
tools/perf-summary.sh --format markdown --label "QStar perf" "$tmp/perf-summary.in" > "$tmp/perf-summary.md" 2> "$tmp/perf-summary-md.err"
contains "$tmp/perf-summary.md" "## QStar perf"
contains "$tmp/perf-summary.md" "| Gate | Mode | Backend | Phase | Count | Min ms | Median ms | Max ms |"
contains "$tmp/perf-summary.md" "| medium | medium | stella | clean | 3 | 130 | 150 | 170 |"
contains "$tmp/perf-summary.md" "| medium | medium | stella-daemon | clean | 3 | 85 | 90 | 95 |"
if tools/perf-summary.sh --ratio-x100 100 --slack-ms 0 --hard "$tmp/perf-summary.in" > "$tmp/perf-summary-hard.out" 2> "$tmp/perf-summary-hard.err"; then
	fail "perf summary hard threshold unexpectedly passed"
fi
contains "$tmp/perf-summary-hard.out" "status=warn"
tools/perf-summary.sh --repeat 2 -- printf '%s\n' \
  "medium_project_gate backend=stella phase=clean elapsed_ms=10" \
  "medium_project_gate backend=ninja phase=clean elapsed_ms=10" \
  > "$tmp/perf-summary-repeat.out" 2> "$tmp/perf-summary-repeat.err"
contains "$tmp/perf-summary-repeat.out" "perf_summary sample gate=medium mode=medium backend=stella phase=clean count=2 min_ms=10 median_ms=10 max_ms=10"
if grep -R -n --include='*.md' -E 'timeout_sec[[:space:]]*=' docs wiki >/dev/null; then
	fail "QStar docs/wiki contain stale timeout_sec authoring examples"
fi
if grep -R -n -E 'qstar\.(uefi_app|rpi_image|embed_binary)' tests/projects/systems-firmware docs wiki >/dev/null; then
	fail "QStar systems corpus/docs contain board-specific builtin"
fi
if grep -R -n -i -E 'ribon|신Ribon|parus|kairon' wiki >/dev/null; then
	fail "QStar wiki must not mention a specific downstream project"
fi

wiki_docs="
wiki/README.md
wiki/getting-started.md
wiki/installation.md
wiki/concepts/workspace-project-package.md
wiki/concepts/labels-and-fragments.md
wiki/concepts/targets-and-actions.md
wiki/concepts/language-namespaces.md
wiki/reference/qstar-lua.md
wiki/reference/target-rules.md
wiki/reference/lang-c.md
wiki/reference/lang-cxx.md
wiki/reference/lang-cale.md
wiki/reference/language-providers.md
wiki/reference/object-artifacts.md
wiki/reference/custom-target.md
wiki/reference/run-target.md
wiki/reference/profiles.md
wiki/reference/diagnostics.md
wiki/tutorials/c-app.md
wiki/tutorials/c-staticlib.md
wiki/tutorials/generated-config.md
wiki/tutorials/cxx-mixed.md
wiki/tutorials/freestanding-image.md
wiki/cookbook/objcopy.md
wiki/cookbook/staging.md
wiki/cookbook/qemu-smoke.md
wiki/cookbook/response-files.md
wiki/migration/from-cmake.md
wiki/migration/from-meson.md
wiki/migration/qstar-v0.2-to-v0.3.md
"
for doc in $wiki_docs; do
	test -f "$doc" || fail "missing QStar wiki document: $doc"
	contains "$doc" "특정 언어에 종속되지 않는 빌드시스템"
	contains "$doc" "## 최소 예제"
	contains "$doc" "## 전체 예제"
	contains "$doc" "## 실패 예제"
	contains "$doc" "## 관련 CLI"
	contains "$doc" "## 관련 diagnostic"
done
if grep -R -n --include='*.md' -E '\.qs\b' wiki >/dev/null; then
	fail "QStar wiki contains stale authoring surface"
fi
for removed_wiki in wiki/authoring-v0.2.md wiki/language-options.md wiki/project-layout.md; do
	if test -e "$removed_wiki"; then
		fail "QStar wiki contains removed summary page: $removed_wiki"
	fi
done
contains "wiki/README.md" "reference/qstar-lua.md"
contains "wiki/reference/qstar-lua.md" "QSTAR_VERSION"
contains "wiki/reference/object-artifacts.md" "format = \"object\""
contains "wiki/reference/custom-target.md" "Object artifact"
contains "wiki/reference/diagnostics.md" "Objective-C provider is not available"
contains "wiki/reference/diagnostics.md" "qstar.output(..., {format = \"object\"})"
contains "docs/ninja-backend-parity.md" "object artifact bridge"
contains "docs/ninja-backend-parity.md" "tests/projects/object-artifact-bridge"
contains "wiki/reference/lang-c.md" "lang.c.public_headers"
contains "wiki/reference/lang-cxx.md" "lang.cxx.modules"
contains "wiki/reference/lang-cale.md" "HCL도 header surface"
contains "wiki/reference/language-providers.md" "Cale source는 Stella-only"
contains "wiki/reference/custom-target.md" "qstar.cli"
contains "wiki/tutorials/freestanding-image.md" "linker_script"
contains "wiki/cookbook/qemu-smoke.md" "qstar.run_target"
contains "wiki/migration/from-cmake.md" "target_include_directories"

step "init templates"
"$qstar" init c-app "$tmp/init-c-app" > "$tmp/init-c-app.out" 2> "$tmp/init-c-app.err"
contains "$tmp/init-c-app.out" "qstar init v1"
contains "$tmp/init-c-app.out" "template c-app"
"$qstar" --file "$tmp/init-c-app/qstar.lua" build //:app > "$tmp/init-c-app-build.out" 2> "$tmp/init-c-app-build.err"
contains "$tmp/init-c-app-build.out" "status ok"
"$tmp/init-c-app/build/qstar/out/___app/app" || fail "init c-app binary failed"

"$qstar" init c-lib "$tmp/init-c-lib" > "$tmp/init-c-lib.out" 2> "$tmp/init-c-lib.err"
contains "$tmp/init-c-lib.out" "template c-lib"
"$qstar" --file "$tmp/init-c-lib/qstar.lua" test //:unit > "$tmp/init-c-lib-test.out" 2> "$tmp/init-c-lib-test.err"
contains "$tmp/init-c-lib-test.out" "test_result label=//:unit status=pass"
"$qstar" --file "$tmp/init-c-lib/qstar.lua" install //:core --prefix "$tmp/init-c-lib-prefix" > "$tmp/init-c-lib-install.out" 2> "$tmp/init-c-lib-install.err"
test -f "$tmp/init-c-lib-prefix/lib/libcore.a" || fail "init c-lib did not install static library"

"$qstar" init generated "$tmp/init-generated" > "$tmp/init-generated.out" 2> "$tmp/init-generated.err"
contains "$tmp/init-generated.out" "template generated"
"$qstar" --file "$tmp/init-generated/qstar.lua" build //:app > "$tmp/init-generated-build.out" 2> "$tmp/init-generated-build.err"
contains "$tmp/init-generated-build.out" "status ok"
test -f "$tmp/init-generated/generated/config.h" || fail "init generated missing config header"
"$tmp/init-generated/build/qstar/out/___app/app" || fail "init generated binary failed"

"$qstar" init mixed-cale "$tmp/init-mixed" > "$tmp/init-mixed.out" 2> "$tmp/init-mixed.err"
contains "$tmp/init-mixed.out" "template mixed-cale"
"$qstar" --file "$tmp/init-mixed/qstar.lua" dry-run //:mixed > "$tmp/init-mixed-dry.out" 2> "$tmp/init-mixed-dry.err"
contains "$tmp/init-mixed-dry.out" "argv=[cale, -c, src/plugin.cale"

if "$qstar" init c-app "$tmp/init-c-app" > "$tmp/init-overwrite.out" 2> "$tmp/init-overwrite.err"; then
	fail "qstar init unexpectedly overwrote existing files"
fi
contains "$tmp/init-overwrite.err" "refuses to overwrite existing file"

"$qstar" --file "$tmp/init-c-lib/qstar.lua" explain //:core > "$tmp/rule-explain.out" 2> "$tmp/rule-explain.err"
contains "$tmp/rule-explain.out" "rule provider=native final_action=archive output_group=libs"
contains "$tmp/rule-explain.out" "source_file path=src/core.c language=c tool=c-compiler provider=c output_group=objects role=compile"

step "depfile handling"
mkdir -p "$tmp/depfile/include" "$tmp/depfile/src"
cat > "$tmp/depfile/include/dep.h" <<'EOF'
#define DEP_VALUE 11
EOF
cat > "$tmp/depfile/src/main.c" <<'EOF'
#include "dep.h"
int main(void) { return DEP_VALUE - 11; }
EOF
cat > "$tmp/depfile/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    c = {
      include_dirs = {"include"},
    },
  },
}
EOF
"$qstar" --file "$tmp/depfile/qstar.lua" build //:app > "$tmp/depfile-first.out" 2> "$tmp/depfile-first.err"
contains "$tmp/depfile-first.out" "status ok"
test -f "$tmp/depfile/build/qstar/out/___app/obj0.d" || fail "missing compiler depfile"
cat > "$tmp/depfile/include/dep.h" <<'EOF'
#define DEP_VALUE 12
EOF
"$qstar" --file "$tmp/depfile/qstar.lua" build //:app --explain-cache > "$tmp/depfile-second.out" 2> "$tmp/depfile-second.err"
contains "$tmp/depfile-second.out" "cache_miss id=//:app:compile:0"
contains "$tmp/depfile-second.out" "reason=depfile-changed"
rm -f "$tmp/depfile/include/dep.h"
if "$qstar" --file "$tmp/depfile/qstar.lua" build //:app > "$tmp/depfile-missing.out" 2> "$tmp/depfile-missing.err"; then
	fail "missing depfile-discovered header unexpectedly succeeded"
fi
contains "$tmp/depfile-missing.err" "depfile-discovered header"

mkdir -p "$tmp/depfile-fallback/src" "$tmp/depfile-fallback/tools"
cat > "$tmp/depfile-fallback/tools/no-dep-cc.sh" <<'EOF'
#!/bin/sh
set -eu
out=
while [ "$#" -gt 0 ]; do
	if [ "$1" = "-o" ]; then
		shift
		out=$1
	fi
	shift || true
done
test -n "$out"
mkdir -p "$(dirname "$out")"
printf 'qstar fake object\n' > "$out"
EOF
chmod +x "$tmp/depfile-fallback/tools/no-dep-cc.sh"
cat > "$tmp/depfile-fallback/src/core.c" <<'EOF'
int core(void) { return 1; }
EOF
cat > "$tmp/depfile-fallback/qstar.lua" <<'EOF'
qstar.profile "default" {
  toolchain = "clang",
  cc = "tools/no-dep-cc.sh",
}

qstar.staticlib "core" {
  sources = {"src/core.c"},
}
EOF
"$qstar" --file "$tmp/depfile-fallback/qstar.lua" build //:core --verbose --progress off > "$tmp/depfile-fallback.out" 2> "$tmp/depfile-fallback.err"
contains "$tmp/depfile-fallback.out" "depfile_fallback id=//:core:compile:0"
contains "$tmp/depfile-fallback.out" "status ok"
test ! -f "$tmp/depfile-fallback/build/qstar/out/___core/obj0.d" || fail "fake compiler unexpectedly wrote depfile"

if command -v c++ >/dev/null 2>&1; then
	step "cxx mixed build"
	mkdir -p "$tmp/cxx/include" "$tmp/cxx/src"
	cat > "$tmp/cxx/include/cpp.hpp" <<'EOF'
#ifndef QSTAR_CXX_FLAG
#error missing C++ flag
#endif
static inline int qstar_cpp_value(void) { return 37 + QSTAR_CXX_FLAG; }
EOF
	cat > "$tmp/cxx/src/cpp.cpp" <<'EOF'
#include "cpp.hpp"
extern "C" int cpp_value(void) { return qstar_cpp_value(); }
EOF
	cat > "$tmp/cxx/src/main.c" <<'EOF'
#ifndef QSTAR_C_FLAG
#error missing C flag
#endif
int cpp_value(void);
int main(void) { return cpp_value() - 42; }
EOF
	cat > "$tmp/cxx/qstar.lua" <<'EOF'
qstar.executable "mixed" {
  sources = {"src/main.c", "src/cpp.cpp"},
  lang = {
    c = {
      include_dirs = {"include"},
      compile_options = {"-DQSTAR_C_FLAG=1"},
    },
    cxx = {
      include_dirs = {"include"},
      compile_options = {"-DQSTAR_CXX_FLAG=5"},
      standard = "c++11",
    },
  },
}
EOF
	"$qstar" --file "$tmp/cxx/qstar.lua" dry-run //:mixed > "$tmp/cxx-dry.out" 2> "$tmp/cxx-dry.err"
	contains "$tmp/cxx-dry.out" "argv=[c++, -c, src/cpp.cpp"
	contains "$tmp/cxx-dry.out" "-std=c++11"
	contains "$tmp/cxx-dry.out" "-DQSTAR_CXX_FLAG=5"
	contains "$tmp/cxx-dry.out" "argv=[c++, -o, build/qstar/out/___mixed/mixed"
	"$qstar" --file "$tmp/cxx/qstar.lua" build //:mixed > "$tmp/cxx-build.out" 2> "$tmp/cxx-build.err"
	contains "$tmp/cxx-build.out" "status ok"
	"$tmp/cxx/build/qstar/out/___mixed/mixed" || fail "cxx mixed binary failed"
	contains "$tmp/cxx/build/qstar/compile_commands.json" "src/cpp.cpp"
fi

step "assembler build"
mkdir -p "$tmp/asm/asm/include" "$tmp/asm/src"
cat > "$tmp/asm/asm/include/asm_value.inc" <<'EOF'
#define QSTAR_ASM_RETURN QSTAR_ASM_VALUE
EOF
cat > "$tmp/asm/asm/value.S" <<'EOF'
#include "asm_value.inc"
.text
#if defined(__aarch64__) || defined(__arm64__)
#if defined(__APPLE__)
.globl _asm_value
.p2align 2
_asm_value:
#else
.globl asm_value
.p2align 2
asm_value:
#endif
	mov w0, #QSTAR_ASM_RETURN
	ret
#elif defined(__x86_64__)
#if defined(__APPLE__)
.globl _asm_value
_asm_value:
#else
.globl asm_value
asm_value:
#endif
	movl $QSTAR_ASM_RETURN, %eax
	ret
#else
#error unsupported qstar asm smoke architecture
#endif
#if defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif
EOF
cat > "$tmp/asm/asm/empty.s" <<'EOF'
.text
EOF
cat > "$tmp/asm/src/main.c" <<'EOF'
int asm_value(void);
int main(void) { return asm_value() == 42 ? 0 : 1; }
EOF
cat > "$tmp/asm/qstar.lua" <<'EOF'
qstar.staticlib "plainasm" {
  sources = {"asm/empty.s"},
}

qstar.executable "asmapp" {
  sources = {"src/main.c", "asm/value.S"},
  lang = {
    asm = {
      include_dirs = {"asm/include"},
      compile_options = {"-DQSTAR_ASM_VALUE=42"},
      preprocess = true,
    },
  },
}

qstar.executable "bad_asm_toolchain" {
  toolchain = "cale",
  sources = {"asm/value.S"},
}
EOF
if ! "$qstar" --file "$tmp/asm/qstar.lua" dry-run //:asmapp > "$tmp/asm-dry.out" 2> "$tmp/asm-dry.err"; then
	cat "$tmp/asm-dry.out" >&2
	cat "$tmp/asm-dry.err" >&2
	fail "asm dry-run failed"
fi
contains "$tmp/asm-dry.out" "language=asm-cpp"
contains "$tmp/asm-dry.out" "argv=[cc, -x, assembler-with-cpp, -c, asm/value.S"
contains "$tmp/asm-dry.out" "-DQSTAR_ASM_VALUE=42"
contains "$tmp/asm-dry.out" "asm/include"
if ! "$qstar" --file "$tmp/asm/qstar.lua" build //:plainasm > "$tmp/asm-plain-build.out" 2> "$tmp/asm-plain-build.err"; then
	cat "$tmp/asm-plain-build.out" >&2
	cat "$tmp/asm-plain-build.err" >&2
	fail "plain assembler build failed"
fi
contains "$tmp/asm-plain-build.out" "status ok"
if ! "$qstar" --file "$tmp/asm/qstar.lua" build //:asmapp > "$tmp/asm-build.out" 2> "$tmp/asm-build.err"; then
	cat "$tmp/asm-build.out" >&2
	cat "$tmp/asm-build.err" >&2
	fail "asm app build failed"
fi
contains "$tmp/asm-build.out" "status ok"
"$tmp/asm/build/qstar/out/___asmapp/asmapp" || fail "asm smoke binary failed"
contains "$tmp/asm/build/qstar/compile_commands.json" "asm/value.S"
contains "$tmp/asm/build/qstar/compile_commands.json" "-x assembler-with-cpp"
if "$qstar" --file "$tmp/asm/qstar.lua" build //:bad_asm_toolchain > "$tmp/asm-bad-toolchain.out" 2> "$tmp/asm-bad-toolchain.err"; then
	fail "assembler with Cale toolchain unexpectedly succeeded"
fi
contains "$tmp/asm-bad-toolchain.err" "assembler source 'asm/value.S' requires host or clang toolchain"

step "cxx module diagnostics"
mkdir -p "$tmp/cxx-module/src"
cat > "$tmp/cxx-module/qstar.lua" <<'EOF'
qstar.executable "bad_module" {
  sources = {"src/module.cppm"},
}
EOF
cat > "$tmp/cxx-module/src/module.cppm" <<'EOF'
export module bad;
EOF
if "$qstar" --file "$tmp/cxx-module/qstar.lua" build //:bad_module > "$tmp/cxx-module.out" 2> "$tmp/cxx-module.err"; then
	fail "C++ module source unexpectedly built"
fi
contains "$tmp/cxx-module.err" "C++ modules are not supported"

step "language namespace surface"
mkdir -p "$tmp/lang-surface/boot/include" "$tmp/lang-surface/src" "$tmp/lang-surface/include"
cat > "$tmp/lang-surface/qstar.lua" <<'EOF'
qstar.staticlib "boot" {
  sources = {"boot/start.S"},
  lang = {
    asm = {
      include_dirs = {"boot/include"},
      compile_options = {"-ffreestanding"},
      preprocess = true,
    },
  },
}

qstar.staticlib "cale_core" {
  toolchain = "cale",
  sources = {"src/core.cl"},
  lang = {
    cale = {
      profile = "safe",
      compile_options = {"--profile=safe"},
      public_headers = {"include/core.hcl"},
      public_include_dirs = {"include"},
    },
  },
}

qstar.staticlib "cxx_mod_shell" {
  sources = {"src/mod.cpp"},
  lang = {
    cxx = {
      modules = { enabled = false },
    },
  },
}
EOF
cat > "$tmp/lang-surface/boot/start.S" <<'EOF'
.globl _start
_start:
EOF
cat > "$tmp/lang-surface/src/core.cl" <<'EOF'
fn core() -> int { return 0; }
EOF
cat > "$tmp/lang-surface/src/mod.cpp" <<'EOF'
int mod_shell(void) { return 0; }
EOF
"$qstar" --file "$tmp/lang-surface/qstar.lua" --dump-graph > "$tmp/lang-surface.out" 2> "$tmp/lang-surface.err"
contains "$tmp/lang-surface.out" "lang.asm.include_dirs [boot/include]"
contains "$tmp/lang-surface.out" "lang.asm.compile_options [-ffreestanding]"
contains "$tmp/lang-surface.out" "lang.asm.preprocess true"
contains "$tmp/lang-surface.out" "public_headers [include/core.hcl]"
contains "$tmp/lang-surface.out" "lang.cale.compile_options [--profile=safe]"
contains "$tmp/lang-surface.out" "lang.cale.profile safe"
contains "$tmp/lang-surface.out" "lang.cxx.modules enabled=false"

step "workspace fragments"
mkdir -p "$tmp/workspace/app/src" "$tmp/workspace/lib/src" "$tmp/workspace/lib/include" "$tmp/workspace/lib/private"
cat > "$tmp/workspace/lib/include/core.h" <<'EOF'
int core_value(void);
EOF
cat > "$tmp/workspace/lib/private/core_private.h" <<'EOF'
#define CORE_PRIVATE 1
EOF
cat > "$tmp/workspace/lib/src/core.c" <<'EOF'
#include "core.h"
int core_value(void) { return 5; }
EOF
cat > "$tmp/workspace/app/src/main.c" <<'EOF'
#include "core.h"
int main(void) { return core_value() - 5; }
EOF
cat > "$tmp/workspace/lib/lib.qst" <<'EOF'
qstar.staticlib "core" {
  sources = {"lib/src/core.c"},
  lang = {
    c = {
      public_headers = {"lib/include/core.h"},
      private_headers = {"lib/private/core_private.h"},
      public_include_dirs = {"lib/include"},
      private_include_dirs = {"lib/private"},
    },
  },
  visibility = {"//app:..."},
}
EOF
cat > "$tmp/workspace/app/app.qst" <<'EOF'
qstar.executable "app" {
  sources = {"app/src/main.c"},
  deps = {"//lib:core"},
}
EOF
cat > "$tmp/workspace/qstar.lua" <<'EOF'
qstar.subdir("lib")
qstar.subdir("app")
EOF
"$qstar" --file "$tmp/workspace/app/app.qst" query //app:app > "$tmp/workspace-query.out" 2> "$tmp/workspace-query.err"
contains "$tmp/workspace-query.out" "target //app:app"
contains "$tmp/workspace-query.out" "package app"
"$qstar" --file "$tmp/workspace/qstar.lua" build //app:app --verbose > "$tmp/workspace-build.out" 2> "$tmp/workspace-build.err"
contains "$tmp/workspace-build.out" "package-root $tmp/workspace"
contains "$tmp/workspace-build.out" "status ok"
"$tmp/workspace/build/qstar/out/__app_app/app"

cat > "$tmp/workspace/app/app.qst" <<'EOF'
qstar.executable "bad_leak" {
  sources = {"app/src/main.c"},
  deps = {"//lib:core"},
  lang = {
    c = {
      include_dirs = {"lib/private"},
    },
  },
}
EOF
if "$qstar" --file "$tmp/workspace/qstar.lua" check //app:bad_leak > "$tmp/private-leak.out" 2> "$tmp/private-leak.err"; then
	fail "private include leakage unexpectedly succeeded"
fi
contains "$tmp/private-leak.err" "leaks private include directory"
if "$qstar" --file "$tmp/workspace/qstar.lua" lint //app:bad_leak > "$tmp/private-leak-lint.out" 2> "$tmp/private-leak-lint.err"; then
	fail "private include leakage lint unexpectedly succeeded"
fi
contains "$tmp/private-leak-lint.out" "QSTAR030"
contains "$tmp/private-leak-lint.out" "leaks private include directory"

cat > "$tmp/workspace/lib/lib.qst" <<'EOF'
qstar.staticlib "core" {
  sources = {"lib/src/core.c"},
  lang = {
    c = {
      public_headers = {"lib/include/core.h"},
      public_include_dirs = {"lib/include"},
    },
  },
  visibility = {"//other:..."},
}
EOF
cat > "$tmp/workspace/app/app.qst" <<'EOF'
qstar.executable "blocked" {
  sources = {"app/src/main.c"},
  deps = {"//lib:core"},
}
EOF
if "$qstar" --file "$tmp/workspace/qstar.lua" check //app:blocked > "$tmp/visibility.out" 2> "$tmp/visibility.err"; then
	fail "visibility violation unexpectedly succeeded"
fi
contains "$tmp/visibility.err" "is not visible"

cat > "$tmp/workspace/app/app.qst" <<'EOF'
qstar.executable "//other:oops" {
  sources = {"app/src/main.c"},
}
EOF
if "$qstar" --file "$tmp/workspace/app/app.qst" check //other:oops > "$tmp/ownership.out" 2> "$tmp/ownership.err"; then
	fail "cross-package ownership unexpectedly succeeded"
fi
contains "$tmp/ownership.err" "owned by package"

cat > "$tmp/workspace/app/app.qst" <<'EOF'
qstar.executable "outside" {
  sources = {"../outside.c"},
}
EOF
if "$qstar" --file "$tmp/workspace/app/app.qst" check //app:outside > "$tmp/outside-source.out" 2> "$tmp/outside-source.err"; then
	fail "outside source path unexpectedly succeeded"
fi
contains "$tmp/outside-source.err" "must be package-relative"

step "profile diagnostics"
mkdir -p "$tmp/profile/src"
cat > "$tmp/profile/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/profile/qstar.lua" <<'EOF'
qstar.profile "custom" {
  toolchain = "clang",
  target = "x86_64-unknown-none-elf",
  stdlib = "none",
  cc = "clang-custom",
  cxx = "clang++-custom",
  cale = "cale-custom",
  ar = "llvm-ar-custom",
  linker = "ld-custom",
  sysroot = "sdk root",
  resource_dir = "resource dir",
  include_dirs = {"profile include", "profinc"},
  lib_dirs = {"profile lib"},
}

qstar.executable "app" {
  sources = {"src/main.c"},
  libs = {"m"},
}
EOF
"$qstar" --file "$tmp/profile/qstar.lua" --profile custom dry-run //:app > "$tmp/profile-dry.out" 2> "$tmp/profile-dry.err"
contains "$tmp/profile-dry.out" "resolved_toolchain owner=//:app toolchain=clang profile=custom target=x86_64-unknown-none-elf stdlib=none resolver=profile-schema-v2 cc=clang-custom"
contains "$tmp/profile-dry.out" "\"--sysroot=sdk root\""
contains "$tmp/profile-dry.out" "-resource-dir"
contains "$tmp/profile-dry.out" "\"resource dir\""
contains "$tmp/profile-dry.out" "\"profile include\""
contains "$tmp/profile-dry.out" "\"-Lprofile lib\""
contains "$tmp/profile-dry.out" "digest="
"$qstar" --file "$tmp/profile/qstar.lua" --profile custom doctor > "$tmp/profile-doctor.out" 2> "$tmp/profile-doctor.err"
contains "$tmp/profile-doctor.out" "profile-schema in-dsl-v1 include_dirs=2 lib_dirs=1"
contains "$tmp/profile-doctor.out" "toolchain-sanity name=clang cc=clang-custom cxx=clang++-custom cale=cale-custom ar=llvm-ar-custom linker=ld-custom"

step "freestanding profile"
mkdir -p "$tmp/freestanding/src" "$tmp/freestanding/tools" "$tmp/freestanding/linker"
cat > "$tmp/freestanding/tools/fake-cc.sh" <<'EOF'
#!/bin/sh
set -eu
out=
dep=
src=
while [ $# -gt 0 ]; do
	case "$1" in
	-o)
		shift
		out=$1
		;;
	-MF)
		shift
		dep=$1
		;;
	-c)
		shift
		src=$1
		;;
	esac
	shift || break
done
mkdir -p "$(dirname "$out")"
printf "fake object\n" > "$out"
if [ -n "$dep" ]; then
	mkdir -p "$(dirname "$dep")"
	printf "%s: %s\n" "$out" "$src" > "$dep"
fi
EOF
cat > "$tmp/freestanding/tools/fake-link.sh" <<'EOF'
#!/bin/sh
set -eu
out=
while [ $# -gt 0 ]; do
	if [ "$1" = "-o" ]; then
		shift
		out=$1
	fi
	shift || break
done
mkdir -p "$(dirname "$out")"
printf "fake link\n" > "$out"
EOF
chmod +x "$tmp/freestanding/tools/fake-cc.sh" "$tmp/freestanding/tools/fake-link.sh"
cat > "$tmp/freestanding/src/kernel.c" <<'EOF'
int kernel_main(void) { return 0; }
EOF
cat > "$tmp/freestanding/linker/profile.ld" <<'EOF'
SECTIONS { . = 0x1000; }
EOF
cat > "$tmp/freestanding/linker/kernel.ld" <<'EOF'
SECTIONS { . = 0x80000; }
EOF
cat > "$tmp/freestanding/qstar.lua" <<'EOF'
qstar.profile "default" {
  toolchain = "clang",
  target = "aarch64-none-elf",
  arch = "aarch64",
  cpu = "cortex-a76",
  abi = "lp64",
  freestanding = true,
  cc = "tools/fake-cc.sh",
  linker = "tools/fake-link.sh",
  linker_script = "linker/profile.ld",
  link_options = {"-nostdlib"},
  defsyms = {"__profile_base=0x1000"},
}

qstar.executable "kernel" {
  sources = {"src/kernel.c"},
  link_options = {"-Wl,-Map=kernel.map"},
  linker_script = "linker/kernel.ld",
  defsyms = {"__stack_top=0x80000"},
}
EOF
"$qstar" --file "$tmp/freestanding/qstar.lua" dry-run //:kernel > "$tmp/freestanding-dry.out" 2> "$tmp/freestanding-dry.err"
contains "$tmp/freestanding-dry.out" "profile_target arch=aarch64 cpu=cortex-a76 abi=lp64 freestanding=true"
contains "$tmp/freestanding-dry.out" "profile_link linker_script=linker/profile.ld link_options=[-nostdlib] defsyms=[__profile_base=0x1000]"
contains "$tmp/freestanding-dry.out" "-ffreestanding"
contains "$tmp/freestanding-dry.out" "-fno-builtin"
contains "$tmp/freestanding-dry.out" "-fno-stack-protector"
contains "$tmp/freestanding-dry.out" "-mgeneral-regs-only"
contains "$tmp/freestanding-dry.out" "-mcpu=cortex-a76"
contains "$tmp/freestanding-dry.out" "-mabi=lp64"
contains "$tmp/freestanding-dry.out" "-nostdlib"
contains "$tmp/freestanding-dry.out" "-Wl,-Map=kernel.map"
contains "$tmp/freestanding-dry.out" "-T"
contains "$tmp/freestanding-dry.out" "linker/kernel.ld"
contains "$tmp/freestanding-dry.out" "--defsym=__profile_base=0x1000"
contains "$tmp/freestanding-dry.out" "--defsym=__stack_top=0x80000"
"$qstar" --file "$tmp/freestanding/qstar.lua" build //:kernel > "$tmp/freestanding-build.out" 2> "$tmp/freestanding-build.err"
contains "$tmp/freestanding-build.out" "status ok"
"$qstar" --file "$tmp/freestanding/qstar.lua" action-log //:kernel:compile:0 > "$tmp/freestanding-compile-log.out" 2> "$tmp/freestanding-compile-log.err"
"$qstar" --file "$tmp/freestanding/qstar.lua" action-log //:kernel:link:0 > "$tmp/freestanding-link-log.out" 2> "$tmp/freestanding-link-log.err"
contains "$tmp/freestanding-compile-log.out" "-ffreestanding"
contains "$tmp/freestanding-compile-log.out" "-mgeneral-regs-only"
contains "$tmp/freestanding-link-log.out" "-T linker/kernel.ld"
contains "$tmp/freestanding-link-log.out" "--defsym=__stack_top=0x80000"
cat > "$tmp/freestanding/linker/kernel.ld" <<'EOF'
SECTIONS { . = 0x90000; }
EOF
"$qstar" --file "$tmp/freestanding/qstar.lua" build //:kernel --explain-cache --verbose > "$tmp/freestanding-rebuild.out" 2> "$tmp/freestanding-rebuild.err"
contains "$tmp/freestanding-rebuild.out" "cache_miss id=//:kernel:link:0"
contains "$tmp/freestanding-rebuild.out" "reason=input-changed"
contains "$tmp/freestanding-rebuild.out" "status=skip"
cat > "$tmp/freestanding/qstar.lua" <<'EOF'
qstar.executable "bad_script" {
  sources = {"src/kernel.c"},
  linker_script = "../escape.ld",
}
EOF
if "$qstar" --file "$tmp/freestanding/qstar.lua" check //:bad_script > "$tmp/freestanding-bad-script.out" 2> "$tmp/freestanding-bad-script.err"; then
	fail "package-escaping linker_script unexpectedly succeeded"
fi
contains "$tmp/freestanding-bad-script.err" "linker_script '../escape.ld' in '//:bad_script' must be package-relative"
cat > "$tmp/freestanding/qstar.lua" <<'EOF'
qstar.executable "bad_defsym" {
  sources = {"src/kernel.c"},
  defsyms = {"BROKEN"},
}
EOF
if "$qstar" --file "$tmp/freestanding/qstar.lua" check //:bad_defsym > "$tmp/freestanding-bad-defsym.out" 2> "$tmp/freestanding-bad-defsym.err"; then
	fail "bad defsym unexpectedly succeeded"
fi
contains "$tmp/freestanding-bad-defsym.err" "defsym 'BROKEN' in '//:bad_defsym' must be NAME=VALUE"
cat > "$tmp/freestanding/qstar.lua" <<'EOF'
qstar.profile "default" {
  toolchain = "clang",
  target = "aarch64-none-elf",
  linker_script = "linker/missing.ld",
}

qstar.executable "missing_profile_script" {
  sources = {"src/kernel.c"},
}
EOF
if "$qstar" --file "$tmp/freestanding/qstar.lua" check //:missing_profile_script > "$tmp/freestanding-missing-profile.out" 2> "$tmp/freestanding-missing-profile.err"; then
	fail "missing profile linker script unexpectedly succeeded"
fi
contains "$tmp/freestanding-missing-profile.err" "profile linker_script 'linker/missing.ld' does not exist"

"$qstar" --file tests/corpus/freestanding/qstar.lua doctor > "$tmp/freestanding-corpus-doctor.out" 2> "$tmp/freestanding-corpus-doctor.err"
contains "$tmp/freestanding-corpus-doctor.out" "profile_response response_files=on response_style=posix"
contains "$tmp/freestanding-corpus-doctor.out" "response-policy configured_files=on configured_style=posix effective_files=on effective_style=posix"
contains "$tmp/freestanding-corpus-doctor.out" "toolchain-tool role=cc name=tools/fake-clang.sh required=true mode=package status=found"
contains "$tmp/freestanding-corpus-doctor.out" "toolchain-tool role=linker name=tools/fake-link.sh required=true mode=package status=found"
contains "$tmp/freestanding-corpus-doctor.out" "profile-path name=sysroot path=sysroot mode=package status=found type=directory"
contains "$tmp/freestanding-corpus-doctor.out" "profile-path name=resource_dir path=resource mode=package status=found type=directory"
contains "$tmp/freestanding-corpus-doctor.out" "external-tool-override name=llvm-objcopy value=tools/fake-objcopy.sh mode=package status=found"
contains "$tmp/freestanding-corpus-doctor.out" "depfile-behavior compiler=tools/fake-clang.sh"
"$qstar" --file tests/corpus/freestanding/qstar.lua explain //:kernel > "$tmp/freestanding-corpus-explain.out" 2> "$tmp/freestanding-corpus-explain.err"
contains "$tmp/freestanding-corpus-explain.out" "profile_response response_files=on response_style=posix"
contains "$tmp/freestanding-corpus-explain.out" "effective_compile_merge owner=//:kernel"
contains "$tmp/freestanding-corpus-explain.out" "auto_options=[-ffreestanding, -fno-builtin, -fno-stack-protector, -mgeneral-regs-only, -mcpu=cortex-a76, -mabi=lp64]"
contains "$tmp/freestanding-corpus-explain.out" "target_c_compile_options=[-std=c23, -Wall, -Wextra, -Werror]"
contains "$tmp/freestanding-corpus-explain.out" "target_system_include_dirs=[sysroot/include]"

step "doctor missing tools"
mkdir -p "$tmp/profile-doctor-missing/src"
cat > "$tmp/profile-doctor-missing/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/profile-doctor-missing/qstar.lua" <<'EOF'
qstar.profile "default" {
  cc = "qstar-missing-cc",
  linker = "qstar-missing-ld",
  sysroot = "missing-sysroot",
  resource_dir = "missing-resource",
  path_tools = {"qstar-missing-objcopy"},
}

qstar.executable "app" {
  sources = {"src/main.c"},
}
EOF
"$qstar" --file "$tmp/profile-doctor-missing/qstar.lua" doctor > "$tmp/profile-doctor-missing.out" 2> "$tmp/profile-doctor-missing.err"
contains "$tmp/profile-doctor-missing.out" "toolchain-tool role=cc name=qstar-missing-cc required=true mode=path status=missing severity=warning"
contains "$tmp/profile-doctor-missing.out" "toolchain-tool role=linker name=qstar-missing-ld required=true mode=path status=missing severity=warning"
contains "$tmp/profile-doctor-missing.out" "profile-path name=sysroot path=missing-sysroot mode=package status=missing"
contains "$tmp/profile-doctor-missing.out" "profile-path name=resource_dir path=missing-resource mode=package status=missing"
contains "$tmp/profile-doctor-missing.out" "external-tool name=qstar-missing-objcopy mode=path status=missing"

step "external tool policy"
mkdir -p "$tmp/exttool/bin" "$tmp/exttool/src" "$tmp/exttool/tools"
cat > "$tmp/exttool/bin/qstar-extgen" <<'EOF'
#!/bin/sh
set -eu
out=$1
mkdir -p "$(dirname "$out")"
printf 'int ext_value(void) { return 3; }\n' > "$out"
EOF
chmod +x "$tmp/exttool/bin/qstar-extgen"
cat > "$tmp/exttool/src/main.c" <<'EOF'
int ext_value(void);
int main(void) { return ext_value() - 3; }
EOF
cat > "$tmp/exttool/qstar.lua" <<'EOF'
qstar.profile "default" {
  path_tools = {"qstar-extgen"},
}

qstar.custom_target "generated_ext" {
  outputs = {qstar.output("generated/ext.c")},
  command = qstar.cli {"qstar-extgen", qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/ext.c"),
  },
}
EOF
PATH="$tmp/exttool/bin:$PATH" "$qstar" --file "$tmp/exttool/qstar.lua" doctor > "$tmp/exttool-doctor.out" 2> "$tmp/exttool-doctor.err"
contains "$tmp/exttool-doctor.out" "profile_external_tools allow_absolute=false path_tools=[qstar-extgen] tool_overrides=[]"
contains "$tmp/exttool-doctor.out" "external-tool-policy path_tools=1 tool_overrides=0 allow_absolute=false"
contains "$tmp/exttool-doctor.out" "external-tool name=qstar-extgen mode=path status=found"
PATH="$tmp/exttool/bin:$PATH" "$qstar" --file "$tmp/exttool/qstar.lua" dry-run //:app > "$tmp/exttool-dry.out" 2> "$tmp/exttool-dry.err"
contains "$tmp/exttool-dry.out" "dry_run_step id=//:generated_ext:generate:0"
contains "$tmp/exttool-dry.out" "tool=qstar-extgen tool_mode=path resolved_tool=qstar-extgen"
contains "$tmp/exttool-dry.out" "command_argv id=//:generated_ext:generate:0"
PATH="$tmp/exttool/bin:$PATH" "$qstar" --file "$tmp/exttool/qstar.lua" build //:app --verbose > "$tmp/exttool-build.out" 2> "$tmp/exttool-build.err"
contains "$tmp/exttool-build.out" "generated_sandbox id=//:generated_ext inputs=package-root outputs=generated-only cwd=package-root network=disabled tool=qstar-extgen tool_mode=path resolved_tool=qstar-extgen"
contains "$tmp/exttool-build.out" "status ok"
PATH="$tmp/exttool/bin:$PATH" "$qstar" --file "$tmp/exttool/qstar.lua" -B build/async-generated build //:app --schedule-trace --progress off --color never > "$tmp/exttool-async.out" 2> "$tmp/exttool-async.err"
contains "$tmp/exttool-async.out" "schedule_action id=//:generated_ext:generate:0 kind=generate slot="
contains "$tmp/exttool-async.out" "parallel_event target=//:generated_ext event=start id=//:generated_ext:generate:0"
PATH="$tmp/exttool/bin:$PATH" "$qstar" --file "$tmp/exttool/qstar.lua" action-log //:generated_ext:generate:0 > "$tmp/exttool-generated-log.out" 2> "$tmp/exttool-generated-log.err"
contains "$tmp/exttool-generated-log.out" "argv[0]=qstar-extgen"
"$tmp/exttool/build/qstar/out/___app/app"

mkdir -p "$tmp/exttool-deny/src"
cat > "$tmp/exttool-deny/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/exttool-deny/qstar.lua" <<'EOF'
qstar.custom_target "generated_ext" {
  outputs = {qstar.output("generated/ext.c")},
  command = qstar.cli {"qstar-extgen", qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/ext.c"),
  },
}
EOF
if "$qstar" --file "$tmp/exttool-deny/qstar.lua" check //:app > "$tmp/exttool-deny.out" 2> "$tmp/exttool-deny.err"; then
	fail "unallowlisted PATH tool unexpectedly succeeded"
fi
contains "$tmp/exttool-deny.err" "generated action PATH tool 'qstar-extgen' is not allowed by profile path_tools"

mkdir -p "$tmp/tool-override/src" "$tmp/tool-override/tools"
cat > "$tmp/tool-override/tools/fake-objcopy.sh" <<'EOF'
#!/bin/sh
set -eu
out=$1
mkdir -p "$(dirname "$out")"
printf 'int override_value(void) { return 4; }\n' > "$out"
EOF
chmod +x "$tmp/tool-override/tools/fake-objcopy.sh"
cat > "$tmp/tool-override/src/main.c" <<'EOF'
int override_value(void);
int main(void) { return override_value() - 4; }
EOF
cat > "$tmp/tool-override/qstar.lua" <<'EOF'
qstar.profile "default" {
  tool_overrides = {"llvm-objcopy=tools/fake-objcopy.sh"},
}

qstar.custom_target "generated_ext" {
  outputs = {qstar.output("generated/override.c")},
  command = qstar.cli {"llvm-objcopy", qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/override.c"),
  },
}
EOF
"$qstar" --file "$tmp/tool-override/qstar.lua" doctor > "$tmp/tool-override-doctor.out" 2> "$tmp/tool-override-doctor.err"
contains "$tmp/tool-override-doctor.out" "external-tool-policy path_tools=0 tool_overrides=1 allow_absolute=false"
contains "$tmp/tool-override-doctor.out" "external-tool-override name=llvm-objcopy value=tools/fake-objcopy.sh mode=package status=found"
"$qstar" --file "$tmp/tool-override/qstar.lua" dry-run //:app > "$tmp/tool-override-dry.out" 2> "$tmp/tool-override-dry.err"
contains "$tmp/tool-override-dry.out" "tool=llvm-objcopy tool_mode=override-package resolved_tool=tools/fake-objcopy.sh"
contains "$tmp/tool-override-dry.out" "argv=[tools/fake-objcopy.sh"
"$qstar" --file "$tmp/tool-override/qstar.lua" build //:app --verbose > "$tmp/tool-override-build.out" 2> "$tmp/tool-override-build.err"
contains "$tmp/tool-override-build.out" "tool=llvm-objcopy tool_mode=override-package resolved_tool=tools/fake-objcopy.sh"
"$qstar" --file "$tmp/tool-override/qstar.lua" action-log //:generated_ext:generate:0 > "$tmp/tool-override-generated-log.out" 2> "$tmp/tool-override-generated-log.err"
contains "$tmp/tool-override-generated-log.out" "argv[0]=tools/fake-objcopy.sh"
"$tmp/tool-override/build/qstar/out/___app/app"

mkdir -p "$tmp/absolute-tool/src"
cat > "$tmp/absolute-tool/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/absolute-tool/qstar.lua" <<EOF
qstar.custom_target "generated_ext" {
  outputs = {qstar.output("generated/ext.c")},
  command = qstar.cli {"$tmp/exttool/bin/qstar-extgen", qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/ext.c"),
  },
}
EOF
if "$qstar" --file "$tmp/absolute-tool/qstar.lua" check //:app > "$tmp/absolute-tool-deny.out" 2> "$tmp/absolute-tool-deny.err"; then
	fail "absolute external tool unexpectedly succeeded without profile capability"
fi
contains "$tmp/absolute-tool-deny.err" "requires allow_absolute_tools=true"
cat > "$tmp/absolute-tool/qstar.lua" <<EOF
qstar.profile "default" {
  allow_absolute_tools = true,
}

qstar.custom_target "generated_ext" {
  outputs = {qstar.output("generated/ext.c")},
  command = qstar.cli {"$tmp/exttool/bin/qstar-extgen", qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/ext.c"),
  },
}
EOF
"$qstar" --file "$tmp/absolute-tool/qstar.lua" dry-run //:app > "$tmp/absolute-tool-dry.out" 2> "$tmp/absolute-tool-dry.err"
contains "$tmp/absolute-tool-dry.out" "tool_mode=absolute"
contains "$tmp/absolute-tool-dry.out" "$tmp/exttool/bin/qstar-extgen"

step "response files"
mkdir -p "$tmp/longcmd/src"
cat > "$tmp/longcmd/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/longcmd/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    c = {
      include_dirs = {
        "include/very/long/path/segment/000",
        "include/very/long/path/segment/001",
        "include/very/long/path/segment/002",
        "include/very/long/path/segment/003",
        "include/very/long/path/segment/004",
        "include/very/long/path/segment/005",
        "include/very/long/path/segment/006",
        "include/very/long/path/segment/007",
        "include/very/long/path/segment/008",
        "include/very/long/path/segment/009",
        "include/very/long/path/segment/010",
        "include/very/long/path/segment/011",
        "include/very/long/path/segment/012",
        "include/very/long/path/segment/013",
        "include/very/long/path/segment/014",
        "include/very/long/path/segment/015",
        "include/very/long/path/segment/016",
        "include/very/long/path/segment/017",
        "include/very/long/path/segment/018",
        "include/very/long/path/segment/019",
      },
    },
  },
}
EOF
"$qstar" --file "$tmp/longcmd/qstar.lua" dry-run //:app > "$tmp/longcmd-dry.out" 2> "$tmp/longcmd-dry.err"
contains "$tmp/longcmd-dry.out" "response=skeleton"
contains "$tmp/longcmd-dry.out" "response_file=build/qstar/rsp/"
contains "$tmp/longcmd-dry.out" "response_style=posix"
contains "$tmp/longcmd-dry.out" "response_digest="
"$qstar" --file "$tmp/longcmd/qstar.lua" build //:app > "$tmp/longcmd-build.out" 2> "$tmp/longcmd-build.err"
contains "$tmp/longcmd-build.out" "response_file id=//:app:compile:0"
contains "$tmp/longcmd-build.out" "style=posix"
contains "$tmp/longcmd-build.out" "digest="
test -d "$tmp/longcmd/build/qstar/rsp" || fail "missing real response file dir"
contains "$tmp/longcmd-build.out" "status ok"

cat > "$tmp/longcmd/src/main.c" <<'EOF'
int main(void) { return (1 + ); }
EOF
if "$qstar" --file "$tmp/longcmd/qstar.lua" build //:app --explain-cache > "$tmp/longcmd-fail.out" 2> "$tmp/longcmd-fail.err"; then
	fail "long response-file failure unexpectedly succeeded"
fi
contains "$tmp/longcmd/build/qstar/logs/last-failure.replay" "qstar failure replay v2"
contains "$tmp/longcmd/build/qstar/logs/last-failure.replay" "argv_digest="
contains "$tmp/longcmd/build/qstar/logs/last-failure.replay" "response_file path=build/qstar/rsp/___app_compile_0.rsp style=posix digest="

mkdir -p "$tmp/rsppolicy/src"
cat > "$tmp/rsppolicy/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/rsppolicy/qstar.lua" <<'EOF'
qstar.profile "default" {
  toolchain = "clang",
  target = "x86_64-unknown-none-elf",
  response_files = "off",
}

qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    c = {
      include_dirs = {
        "include/very/long/path/segment/000",
        "include/very/long/path/segment/001",
        "include/very/long/path/segment/002",
        "include/very/long/path/segment/003",
        "include/very/long/path/segment/004",
        "include/very/long/path/segment/005",
        "include/very/long/path/segment/006",
        "include/very/long/path/segment/007",
        "include/very/long/path/segment/008",
        "include/very/long/path/segment/009",
        "include/very/long/path/segment/010",
        "include/very/long/path/segment/011",
      },
    },
  },
}
EOF
"$qstar" --file "$tmp/rsppolicy/qstar.lua" dry-run //:app > "$tmp/rsppolicy-dry.out" 2> "$tmp/rsppolicy-dry.err"
contains "$tmp/rsppolicy-dry.out" "response=unsupported response_capability=off"
contains "$tmp/rsppolicy-dry.out" "response_files=off response_style=posix"

mkdir -p "$tmp/windows/src"
cat > "$tmp/windows/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/windows/qstar.lua" <<'EOF'
qstar.profile "default" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "clang-cl",
  cxx = "clang-cl",
  linker = "clang-cl",
  response_style = "msvc",
}

qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    c = {
      include_dirs = {
        "win include dir",
        "sdk/include/tail",
        "include/very/long/path/segment/000",
        "include/very/long/path/segment/001",
        "include/very/long/path/segment/002",
        "include/very/long/path/segment/003",
        "include/very/long/path/segment/004",
        "include/very/long/path/segment/005",
        "include/very/long/path/segment/006",
        "include/very/long/path/segment/007",
        "include/very/long/path/segment/008",
        "include/very/long/path/segment/009",
      },
    },
  },
  lib_dirs = {"win lib"},
  libs = {"user32"},
}
EOF
"$qstar" --file "$tmp/windows/qstar.lua" dry-run //:app > "$tmp/windows-dry.out" 2> "$tmp/windows-dry.err"
contains "$tmp/windows-dry.out" "response_style=msvc"
contains "$tmp/windows-dry.out" "response_digest="
contains "$tmp/windows-dry.out" "/link"
contains "$tmp/windows-dry.out" "/LIBPATH:win lib"
contains "$tmp/windows-dry.out" "user32.lib"

step "artifact metadata"
mkdir -p "$tmp/artifact/tools" "$tmp/artifact/fixtures"
cat > "$tmp/artifact/tools/fake-objcopy.sh" <<'EOF'
#!/bin/sh
set -eu
fmt=
in=
out=
while [ "$#" -gt 0 ]; do
	case "$1" in
		-O)
			shift
			fmt=$1
			;;
		*)
			if [ -z "${in:-}" ]; then
				in=$1
			else
				out=$1
			fi
			;;
	esac
	shift
done
test "$fmt" = binary
test -n "$in"
test -n "$out"
mkdir -p "$(dirname "$out")"
cp "$in" "$out"
EOF
chmod +x "$tmp/artifact/tools/fake-objcopy.sh"
cat > "$tmp/artifact/fixtures/kernel.elf" <<'EOF'
ELF-STUB
payload
EOF
cat > "$tmp/artifact/qstar.lua" <<'EOF'
qstar.profile "default" {
  tool_overrides = {"llvm-objcopy=tools/fake-objcopy.sh"},
}

qstar.custom_target "kernel_img" {
  inputs = {"fixtures/kernel.elf"},
  outputs = {
    qstar.output("generated/kernel8.img", {
      group = "images",
      format = "raw-binary",
      address = "0x80000",
      layout = "rpi5-kernel8",
    }),
  },
  command = qstar.cli {
    "llvm-objcopy",
    "-O",
    "binary",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.run_target "smoke" {
  command = qstar.cli {"tools/fake-objcopy.sh", "-O", "binary", qstar.target_file("//:kernel_img"), "generated/copy.img"},
}
EOF
"$qstar" --file "$tmp/artifact/qstar.lua" check //:kernel_img > "$tmp/artifact-check.out" 2> "$tmp/artifact-check.err"
contains "$tmp/artifact-check.out" "generated-action-count 1"
"$qstar" --file "$tmp/artifact/qstar.lua" explain //:kernel_img > "$tmp/artifact-explain.out" 2> "$tmp/artifact-explain.err"
contains "$tmp/artifact-explain.out" "plan_generated_action //:kernel_img"
contains "$tmp/artifact-explain.out" "generated_artifact output=generated/kernel8.img group=images format=raw-binary address=0x80000 layout=rpi5-kernel8"
contains "$tmp/artifact-explain.out" "identity=generated/kernel8.img|group=images|format=raw-binary|address=0x80000|layout=rpi5-kernel8"
"$qstar" --file "$tmp/artifact/qstar.lua" dry-run //:kernel_img > "$tmp/artifact-dry.out" 2> "$tmp/artifact-dry.err"
contains "$tmp/artifact-dry.out" "dry_run_generated_action //:kernel_img"
contains "$tmp/artifact-dry.out" "tool=llvm-objcopy tool_mode=override-package resolved_tool=tools/fake-objcopy.sh"
contains "$tmp/artifact-dry.out" "argv=[tools/fake-objcopy.sh, -O, binary, fixtures/kernel.elf, generated/kernel8.img]"
"$qstar" --file "$tmp/artifact/qstar.lua" build //:kernel_img --verbose > "$tmp/artifact-build.out" 2> "$tmp/artifact-build.err"
contains "$tmp/artifact-build.out" "build_generated_action //:kernel_img"
contains "$tmp/artifact-build.out" "output_identity=[generated/kernel8.img|group=images|format=raw-binary|address=0x80000|layout=rpi5-kernel8]"
contains "$tmp/artifact-build.out" "status ok"
test -f "$tmp/artifact/generated/kernel8.img" || fail "missing generated raw image artifact"
cmp "$tmp/artifact/fixtures/kernel.elf" "$tmp/artifact/generated/kernel8.img" >/dev/null || fail "raw image artifact content drifted"
test ! -e "$tmp/artifact/build/qstar/state/graph.json" || fail "artifact graph snapshot should be debug opt-in"
test ! -e "$tmp/artifact/build/qstar/state/actions.json" || fail "artifact debug action state dump should be opt-in"
QSTAR_DEBUG_STATE_DUMPS=1 "$qstar" --file "$tmp/artifact/qstar.lua" build //:kernel_img --progress off > "$tmp/artifact-debug-state.out" 2> "$tmp/artifact-debug-state.err"
contains "$tmp/artifact/build/qstar/state/graph.json" "\"output_artifacts\""
contains "$tmp/artifact/build/qstar/state/graph.json" "\"format\":\"raw-binary\""
contains "$tmp/artifact/build/qstar/state/actions.json" "\"output\":\"generated/kernel8.img\""
"$qstar" --file "$tmp/artifact/qstar.lua" action-log //:kernel_img:generate:0 > "$tmp/artifact-img-log.out" 2> "$tmp/artifact-img-log.err"
contains "$tmp/artifact-img-log.out" "argv[0]=tools/fake-objcopy.sh"
"$qstar" --file "$tmp/artifact/qstar.lua" build //:smoke > "$tmp/artifact-smoke.out" 2> "$tmp/artifact-smoke.err"
contains "$tmp/artifact-smoke.out" "run_target label=//:smoke"
test -f "$tmp/artifact/generated/copy.img" || fail "target_file generated artifact path was not consumed"
"$qstar" --file "$tmp/artifact/qstar.lua" list-targets --format json > "$tmp/artifact-targets-json.out" 2> "$tmp/artifact-targets-json.err"
contains "$tmp/artifact-targets-json.out" "\"output_artifacts\""
contains "$tmp/artifact-targets-json.out" "\"group\":\"images\""
contains "$tmp/artifact-targets-json.out" "\"format\":\"raw-binary\""

step "artifact dependency edges"
mkdir -p "$tmp/artifact-dep/tools" "$tmp/artifact-dep/src" "$tmp/artifact-dep/fixtures"
cat > "$tmp/artifact-dep/tools/fake-objcopy.sh" <<'EOF'
#!/bin/sh
set -eu
fmt=
in=
out=
while [ "$#" -gt 0 ]; do
	case "$1" in
		-O)
			shift
			fmt=$1
			;;
		*)
			if [ -z "${in:-}" ]; then
				in=$1
			else
				out=$1
			fi
			;;
	esac
	shift
done
test "$fmt" = binary
test -f "$in"
mkdir -p "$(dirname "$out")"
cp "$in" "$out"
EOF
cat > "$tmp/artifact-dep/tools/copy.sh" <<'EOF'
#!/bin/sh
set -eu
test -f "$1"
mkdir -p "$(dirname "$2")"
cp "$1" "$2"
EOF
chmod +x "$tmp/artifact-dep/tools/fake-objcopy.sh" "$tmp/artifact-dep/tools/copy.sh"
cat > "$tmp/artifact-dep/src/kernel.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/artifact-dep/fixtures/blob.bin" <<'EOF'
BLOB-V1
EOF
cat > "$tmp/artifact-dep/qstar.lua" <<'EOF'
qstar.profile "default" {
  tool_overrides = {"llvm-objcopy=tools/fake-objcopy.sh"},
}

qstar.executable "kernel" {
  sources = {"src/kernel.c"},
}

qstar.custom_target "kernel_img" {
  inputs = {
    qstar.target_file("//:kernel"),
  },
  outputs = {
    qstar.output("generated/kernel.img", {
      group = "images",
      format = "raw-binary",
      layout = "host-kernel-image",
    }),
  },
  command = qstar.cli {"llvm-objcopy", "-O", "binary", qstar.input(0), qstar.output(0)},
}

qstar.custom_target "raw_blob" {
  inputs = {"fixtures/blob.bin"},
  outputs = {qstar.output("generated/blob.raw", {format = "raw-binary"})},
  command = qstar.cli {"tools/copy.sh", qstar.input(0), qstar.output(0)},
}

qstar.custom_target "blob_image" {
  inputs = {
    qstar.target_file("//:raw_blob"),
  },
  outputs = {qstar.output("generated/blob.img", {group = "images", format = "raw-binary"})},
  command = qstar.cli {"tools/copy.sh", qstar.input(0), qstar.output(0)},
}
EOF
"$qstar" --file "$tmp/artifact-dep/qstar.lua" explain //:kernel_img > "$tmp/artifact-dep-explain.out" 2> "$tmp/artifact-dep-explain.err"
contains "$tmp/artifact-dep-explain.out" "artifact_input_edge input=<qstar-target-file://:kernel> producer=//:kernel path="
"$qstar" --file "$tmp/artifact-dep/qstar.lua" dry-run //:kernel_img > "$tmp/artifact-dep-dry.out" 2> "$tmp/artifact-dep-dry.err"
contains "$tmp/artifact-dep-dry.out" "artifact_input_edge input=<qstar-target-file://:kernel> producer=//:kernel path="
contains "$tmp/artifact-dep-dry.out" "argv=[tools/fake-objcopy.sh, -O, binary, build/qstar/out/"
"$qstar" --file "$tmp/artifact-dep/qstar.lua" build //:kernel_img --schedule-trace > "$tmp/artifact-dep-build.out" 2> "$tmp/artifact-dep-build.err"
contains "$tmp/artifact-dep-build.out" "build_target //:kernel"
contains "$tmp/artifact-dep-build.out" "build_generated_action //:kernel_img"
contains "$tmp/artifact-dep-build.out" "status ok"
test -s "$tmp/artifact-dep/generated/kernel.img" || fail "target_file compile artifact input did not produce image"
"$qstar" --file "$tmp/artifact-dep/qstar.lua" build //:blob_image --verbose > "$tmp/artifact-dep-blob.out" 2> "$tmp/artifact-dep-blob.err"
contains "$tmp/artifact-dep-blob.out" "generated_sandbox id=//:raw_blob"
contains "$tmp/artifact-dep-blob.out" "build_generated_action //:blob_image"
cmp "$tmp/artifact-dep/fixtures/blob.bin" "$tmp/artifact-dep/generated/blob.img" >/dev/null || fail "target_file generated artifact input content drifted"

mkdir -p "$tmp/artifact-unknown"
cat > "$tmp/artifact-unknown/qstar.lua" <<'EOF'
qstar.custom_target "bad" {
  inputs = {
    qstar.target_file("//:missing"),
  },
  outputs = {qstar.output("generated/bad.img")},
  command = qstar.cli {"tools/missing.sh", qstar.input(0), qstar.output(0)},
}
EOF
if "$qstar" --file "$tmp/artifact-unknown/qstar.lua" check > "$tmp/artifact-unknown.out" 2> "$tmp/artifact-unknown.err"; then
	fail "unknown target_file input unexpectedly succeeded"
fi
contains "$tmp/artifact-unknown.err" "generated input target '//:missing' in '//:bad' is unknown"

mkdir -p "$tmp/artifact-collision/tools" "$tmp/artifact-collision/fixtures"
cp "$tmp/artifact/tools/fake-objcopy.sh" "$tmp/artifact-collision/tools/fake-objcopy.sh"
cp "$tmp/artifact/fixtures/kernel.elf" "$tmp/artifact-collision/fixtures/kernel.elf"
cat > "$tmp/artifact-collision/qstar.lua" <<'EOF'
qstar.profile "default" {
  tool_overrides = {"llvm-objcopy=tools/fake-objcopy.sh"},
}

qstar.custom_target "one" {
  inputs = {"fixtures/kernel.elf"},
  outputs = {qstar.output("generated/one.img", {format = "raw-binary", address = "0x80000", layout = "rpi5-kernel8"})},
  command = qstar.cli {"llvm-objcopy", "-O", "binary", qstar.input(0), qstar.output(0)},
}

qstar.custom_target "two" {
  inputs = {"fixtures/kernel.elf"},
  outputs = {qstar.output("generated/two.img", {group = "images", format = "raw-binary", address = "0x80000", layout = "rpi5-kernel8"})},
  command = qstar.cli {"llvm-objcopy", "-O", "binary", qstar.input(0), qstar.output(0)},
}
EOF
if "$qstar" --file "$tmp/artifact-collision/qstar.lua" check > "$tmp/artifact-collision.out" 2> "$tmp/artifact-collision.err"; then
	fail "duplicate artifact identity unexpectedly succeeded"
fi
contains "$tmp/artifact-collision.err" "generated artifact identity group=images format=raw-binary address=0x80000 layout=rpi5-kernel8 has multiple outputs"

mkdir -p "$tmp/artifact-badmeta"
cat > "$tmp/artifact-badmeta/qstar.lua" <<'EOF'
qstar.custom_target "bad" {
  outputs = {qstar.output("generated/bad.img", {unknown = "x"})},
  command = qstar.cli {"tools/fake.sh", qstar.output(0)},
}
EOF
if "$qstar" --file "$tmp/artifact-badmeta/qstar.lua" check > "$tmp/artifact-badmeta.out" 2> "$tmp/artifact-badmeta.err"; then
	fail "unknown artifact metadata unexpectedly succeeded"
fi
contains "$tmp/artifact-badmeta.err" "unknown qstar.output metadata field 'unknown'"
cat > "$tmp/artifact-badmeta/qstar.lua" <<'EOF'
qstar.custom_target "bad" {
  outputs = {qstar.output("generated/bad.img", {format = true})},
  command = qstar.cli {"tools/fake.sh", qstar.output(0)},
}
EOF
if "$qstar" --file "$tmp/artifact-badmeta/qstar.lua" check > "$tmp/artifact-badmeta-type.out" 2> "$tmp/artifact-badmeta-type.err"; then
	fail "non-string artifact metadata unexpectedly succeeded"
fi
contains "$tmp/artifact-badmeta-type.err" "qstar.output metadata field 'format' must be a string"

step "generated assembly blob"
mkdir -p "$tmp/blob-embed/tools" "$tmp/blob-embed/fixtures" "$tmp/blob-embed/src"
cat > "$tmp/blob-embed/fixtures/payload.elf" <<'EOF'
ELF-FIXTURE-V1
payload
EOF
cat > "$tmp/blob-embed/tools/embed-asm.sh" <<'EOF'
#!/bin/sh
set -eu
input=$1
output=$2
bytes=$(wc -c < "$input" | tr -d ' ')
mkdir -p "$(dirname "$output")"
{
	printf '/* qstar binary blob assembly placeholder */\n'
	printf '/* input=%s bytes=%s */\n' "$input" "$bytes"
} > "$output"
EOF
chmod +x "$tmp/blob-embed/tools/embed-asm.sh"
cat > "$tmp/blob-embed/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/blob-embed/qstar.lua" <<'EOF'
qstar.custom_target "embed_asm" {
  inputs = {"fixtures/payload.elf"},
  outputs = {
    qstar.output("generated/blob.S", {
      group = "objects",
      format = "assembly",
      layout = "rpi5-elf-fixture-embed",
    }),
  },
  command = qstar.cli {"tools/embed-asm.sh", qstar.input(0), qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/blob.S"),
  },
}
EOF
"$qstar" --file "$tmp/blob-embed/qstar.lua" explain //:app > "$tmp/blob-embed-explain.out" 2> "$tmp/blob-embed-explain.err"
contains "$tmp/blob-embed-explain.out" "generated_edge source=generated/blob.S generator=//:embed_asm"
contains "$tmp/blob-embed-explain.out" "generated_artifact output=generated/blob.S group=objects format=assembly"
"$qstar" --file "$tmp/blob-embed/qstar.lua" build //:app --explain-cache --verbose > "$tmp/blob-embed-first.out" 2> "$tmp/blob-embed-first.err"
contains "$tmp/blob-embed-first.out" "build_action id=//:embed_asm:generate:0 status=run"
contains "$tmp/blob-embed-first.out" "build_action id=//:app:compile:1 status=run"
contains "$tmp/blob-embed-first.out" "status ok"
test -f "$tmp/blob-embed/generated/blob.S" || fail "missing generated blob assembly"
"$qstar" --file "$tmp/blob-embed/qstar.lua" build //:app --explain-cache --verbose > "$tmp/blob-embed-second.out" 2> "$tmp/blob-embed-second.err"
contains "$tmp/blob-embed-second.out" "build_action id=//:embed_asm:generate:0 status=skip"
contains "$tmp/blob-embed-second.out" "build_action id=//:app:compile:1 status=skip"
printf 'payload-v2\n' >> "$tmp/blob-embed/fixtures/payload.elf"
"$qstar" --file "$tmp/blob-embed/qstar.lua" build //:app --explain-cache > "$tmp/blob-embed-third.out" 2> "$tmp/blob-embed-third.err"
contains "$tmp/blob-embed-third.out" "cache_miss id=//:embed_asm:generate:0 reason=input-changed"
contains "$tmp/blob-embed-third.out" "cache_miss id=//:app:compile:1 reason=depfile-changed"

step "generated object blob"
mkdir -p "$tmp/blob-object/tools" "$tmp/blob-object/fixtures" "$tmp/blob-object/src"
cat > "$tmp/blob-object/fixtures/payload.elf" <<'EOF'
ELF-FIXTURE-OBJECT-V1
payload
EOF
cat > "$tmp/blob-object/tools/embed-object.sh" <<'EOF'
#!/bin/sh
set -eu
input=$1
output=$2
bytes=$(wc -c < "$input" | tr -d ' ')
mkdir -p "$(dirname "$output")"
tmp="${output}.c"
cat > "$tmp" <<EOF_C
int qstar_embedded_blob_len = $bytes;
EOF_C
${CC:-cc} -c "$tmp" -o "$output"
EOF
chmod +x "$tmp/blob-object/tools/embed-object.sh"
cat > "$tmp/blob-object/src/main.c" <<'EOF'
extern int qstar_embedded_blob_len;
int main(void) { return qstar_embedded_blob_len > 0 ? 0 : 1; }
EOF
cat > "$tmp/blob-object/qstar.lua" <<'EOF'
qstar.custom_target "embed_object" {
  inputs = {"fixtures/payload.elf"},
  outputs = {
    qstar.output("generated/blob.o", {
      format = "object",
      layout = "rpi5-elf-fixture-embed",
    }),
  },
  command = qstar.cli {"tools/embed-object.sh", qstar.input(0), qstar.output(0)},
}

qstar.executable "objapp" {
  sources = {
    "src/main.c",
    qstar.output("generated/blob.o"),
  },
}
EOF
"$qstar" --file "$tmp/blob-object/qstar.lua" explain //:objapp > "$tmp/blob-object-explain.out" 2> "$tmp/blob-object-explain.err"
contains "$tmp/blob-object-explain.out" "generated_artifact output=generated/blob.o group=objects format=object"
contains "$tmp/blob-object-explain.out" "action link-input source=generated/blob.o language=object output=generated/blob.o"
"$qstar" --file "$tmp/blob-object/qstar.lua" dry-run //:objapp > "$tmp/blob-object-dry.out" 2> "$tmp/blob-object-dry.err"
contains "$tmp/blob-object-dry.out" "dry_run_step id=//:objapp:link-input:1 owner=//:objapp kind=link-input language=object"
"$qstar" --file "$tmp/blob-object/qstar.lua" build //:objapp --explain-cache --verbose > "$tmp/blob-object-first.out" 2> "$tmp/blob-object-first.err"
contains "$tmp/blob-object-first.out" "build_action id=//:embed_object:generate:0 status=run"
contains "$tmp/blob-object-first.out" "build_action id=//:objapp:link:0 status=run"
contains "$tmp/blob-object-first.out" "status ok"
"$tmp/blob-object/build/qstar/out/___objapp/objapp"
"$qstar" --file "$tmp/blob-object/qstar.lua" build //:objapp --explain-cache --verbose > "$tmp/blob-object-second.out" 2> "$tmp/blob-object-second.err"
contains "$tmp/blob-object-second.out" "build_action id=//:embed_object:generate:0 status=skip"
contains "$tmp/blob-object-second.out" "build_action id=//:objapp:link:0 status=skip"
printf 'payload-v2\n' >> "$tmp/blob-object/fixtures/payload.elf"
"$qstar" --file "$tmp/blob-object/qstar.lua" build //:objapp --explain-cache > "$tmp/blob-object-third.out" 2> "$tmp/blob-object-third.err"
contains "$tmp/blob-object-third.out" "cache_miss id=//:embed_object:generate:0 reason=input-changed"
contains "$tmp/blob-object-third.out" "cache_miss id=//:objapp:link:0 reason=input-changed"

step "uefi artifact naming"
mkdir -p "$tmp/uefi/src" "$tmp/uefi/tools"
cat > "$tmp/uefi/tools/fake-clang.sh" <<'EOF'
#!/bin/sh
set -eu
out=
dep=
src=
while [ "$#" -gt 0 ]; do
	case "$1" in
		-o)
			shift
			out=$1
			;;
		-MF)
			shift
			dep=$1
			;;
		-c)
			shift
			src=$1
			;;
	esac
	shift || break
done
test -n "$out"
mkdir -p "$(dirname "$out")"
printf "pe-coff-object\n" > "$out"
if [ -n "$dep" ]; then
	mkdir -p "$(dirname "$dep")"
	printf "%s: %s\n" "$out" "$src" > "$dep"
fi
EOF
cat > "$tmp/uefi/tools/fake-lld-link.sh" <<'EOF'
#!/bin/sh
set -eu
out=
subsystem=0
entry=0
nodefault=0
for arg in "$@"; do
	case "$arg" in
		/out:*)
			out=${arg#/out:}
			;;
		/subsystem:efi_application)
			subsystem=1
			;;
		/entry:efi_main)
			entry=1
			;;
		/nodefaultlib)
			nodefault=1
			;;
	esac
done
test -n "$out"
test "$subsystem" = 1
test "$entry" = 1
test "$nodefault" = 1
mkdir -p "$(dirname "$out")"
printf "MZ\nUEFI\n" > "$out"
EOF
chmod +x "$tmp/uefi/tools/fake-clang.sh" "$tmp/uefi/tools/fake-lld-link.sh"
cat > "$tmp/uefi/src/efi_main.c" <<'EOF'
int efi_main(void *image, void *system_table) {
	(void)image;
	(void)system_table;
	return 0;
}
EOF
cat > "$tmp/uefi/qstar.lua" <<'EOF'
qstar.profile "default" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "tools/fake-clang.sh",
  linker = "tools/fake-lld-link.sh",
  response_style = "msvc",
  artifact_names = {"//:boot=BOOTX64.EFI"},
}

qstar.executable "boot" {
  sources = {"src/efi_main.c"},
  lang = {
    c = {
      compile_options = {"-ffreestanding"},
    },
  },
  link_options = {
    "/subsystem:efi_application",
    "/entry:efi_main",
    "/nodefaultlib",
  },
}
EOF
"$qstar" --file "$tmp/uefi/qstar.lua" dry-run //:boot > "$tmp/uefi-x64-dry.out" 2> "$tmp/uefi-x64-dry.err"
contains "$tmp/uefi-x64-dry.out" "response_style=msvc"
contains "$tmp/uefi-x64-dry.out" "/out:build/qstar/out/___boot/BOOTX64.EFI"
contains "$tmp/uefi-x64-dry.out" "/subsystem:efi_application"
contains "$tmp/uefi-x64-dry.out" "/entry:efi_main"
contains "$tmp/uefi-x64-dry.out" "/nodefaultlib"
"$qstar" --file "$tmp/uefi/qstar.lua" build //:boot > "$tmp/uefi-x64-build.out" 2> "$tmp/uefi-x64-build.err"
contains "$tmp/uefi-x64-build.out" "status ok"
test -f "$tmp/uefi/build/qstar/out/___boot/BOOTX64.EFI" || fail "missing UEFI x64 artifact"
"$qstar" --file "$tmp/uefi/qstar.lua" action-log //:boot:link:0 > "$tmp/uefi-x64-link-log.out" 2> "$tmp/uefi-x64-link-log.err"
contains "$tmp/uefi-x64-link-log.out" "argv[0]=tools/fake-lld-link.sh"
contains "$tmp/uefi-x64-link-log.out" "argv[1]=/out:build/qstar/out/___boot/BOOTX64.EFI"
contains "$tmp/uefi-x64-link-log.out" "argv[2]=/subsystem:efi_application"
QSTAR_DEBUG_STATE_DUMPS=1 "$qstar" --file "$tmp/uefi/qstar.lua" build //:boot --progress off > "$tmp/uefi-x64-debug-state.out" 2> "$tmp/uefi-x64-debug-state.err"
contains "$tmp/uefi/build/qstar/state/graph.json" "\"artifact_name\":\"\""
cat > "$tmp/uefi/qstar.lua" <<'EOF'
qstar.profile "default" {
  toolchain = "clang",
  target = "aarch64-pc-windows-msvc",
  cc = "tools/fake-clang.sh",
  linker = "tools/fake-lld-link.sh",
  response_style = "msvc",
  artifact_names = {"//:boot=BOOTAA64.EFI"},
}

qstar.executable "boot" {
  sources = {"src/efi_main.c"},
  lang = {
    c = {
      compile_options = {"-ffreestanding"},
    },
  },
  link_options = {
    "/subsystem:efi_application",
    "/entry:efi_main",
    "/nodefaultlib",
  },
}
EOF
"$qstar" --file "$tmp/uefi/qstar.lua" dry-run //:boot > "$tmp/uefi-aa64-dry.out" 2> "$tmp/uefi-aa64-dry.err"
contains "$tmp/uefi-aa64-dry.out" "/out:build/qstar/out/___boot/BOOTAA64.EFI"
"$qstar" --file "$tmp/uefi/qstar.lua" build //:boot > "$tmp/uefi-aa64-build.out" 2> "$tmp/uefi-aa64-build.err"
contains "$tmp/uefi-aa64-build.out" "status ok"
test -f "$tmp/uefi/build/qstar/out/___boot/BOOTAA64.EFI" || fail "missing UEFI AArch64 artifact"
cat > "$tmp/uefi/qstar.lua" <<'EOF'
qstar.profile "default" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "tools/fake-clang.sh",
  linker = "tools/fake-lld-link.sh",
  response_style = "msvc",
  artifact_names = {"//:boot=BOOTX64.EFI"},
}

qstar.executable "boot" {
  artifact_name = "BOOTLOCAL.EFI",
  sources = {"src/efi_main.c"},
  link_options = {
    "/subsystem:efi_application",
    "/entry:efi_main",
    "/nodefaultlib",
  },
}
EOF
"$qstar" --file "$tmp/uefi/qstar.lua" dry-run //:boot > "$tmp/uefi-local-dry.out" 2> "$tmp/uefi-local-dry.err"
contains "$tmp/uefi-local-dry.out" "/out:build/qstar/out/___boot/BOOTLOCAL.EFI"
"$qstar" --file "$tmp/uefi/qstar.lua" list-targets --format json > "$tmp/uefi-targets-json.out" 2> "$tmp/uefi-targets-json.err"
contains "$tmp/uefi-targets-json.out" "\"artifact_name\":\"BOOTLOCAL.EFI\""
cat > "$tmp/uefi/qstar.lua" <<'EOF'
qstar.profile "default" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "tools/fake-clang.sh",
  linker = "tools/fake-lld-link.sh",
}

qstar.executable "boot" {
  artifact_name = "EFI/BOOT/BOOTX64.EFI",
  sources = {"src/efi_main.c"},
}
EOF
if "$qstar" --file "$tmp/uefi/qstar.lua" check //:boot > "$tmp/uefi-bad-target-name.out" 2> "$tmp/uefi-bad-target-name.err"; then
	fail "path-like artifact_name unexpectedly succeeded"
fi
contains "$tmp/uefi-bad-target-name.err" "artifact_name 'EFI/BOOT/BOOTX64.EFI' must be a filename, not a path"
cat > "$tmp/uefi/qstar.lua" <<'EOF'
qstar.profile "default" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "tools/fake-clang.sh",
  linker = "tools/fake-lld-link.sh",
  artifact_names = {"//:boot=EFI/BOOT/BOOTX64.EFI"},
}

qstar.executable "boot" {
  sources = {"src/efi_main.c"},
}
EOF
if "$qstar" --file "$tmp/uefi/qstar.lua" check //:boot > "$tmp/uefi-bad-profile-name.out" 2> "$tmp/uefi-bad-profile-name.err"; then
	fail "path-like profile artifact_names unexpectedly succeeded"
fi
contains "$tmp/uefi-bad-profile-name.err" "profile artifact_names entry '//:boot=EFI/BOOT/BOOTX64.EFI' must be LABEL=FILENAME"

step "stage package"
mkdir -p "$tmp/stagepkg/src" "$tmp/stagepkg/tools" "$tmp/stagepkg/fixtures" "$tmp/stagepkg/boot"
cat > "$tmp/stagepkg/tools/fake-clang.sh" <<'EOF'
#!/bin/sh
set -eu
out=
dep=
src=
while [ "$#" -gt 0 ]; do
	case "$1" in
		-o)
			shift
			out=$1
			;;
		-MF)
			shift
			dep=$1
			;;
		-c)
			shift
			src=$1
			;;
	esac
	shift || break
done
test -n "$out"
mkdir -p "$(dirname "$out")"
printf "boot-object\n" > "$out"
if [ -n "$dep" ]; then
	mkdir -p "$(dirname "$dep")"
	printf "%s: %s\n" "$out" "$src" > "$dep"
fi
EOF
cat > "$tmp/stagepkg/tools/fake-link.sh" <<'EOF'
#!/bin/sh
set -eu
out=
while [ "$#" -gt 0 ]; do
	case "$1" in
		-o)
			shift
			out=$1
			;;
		/out:*)
			out=${1#/out:}
			;;
	esac
	shift || break
done
test -n "$out"
mkdir -p "$(dirname "$out")"
printf "MZ\nBOOT\n" > "$out"
EOF
cat > "$tmp/stagepkg/tools/fake-objcopy.sh" <<'EOF'
#!/bin/sh
set -eu
format=
input=
output=
while [ "$#" -gt 0 ]; do
	case "$1" in
		-O)
			shift
			format=$1
			;;
		*)
			if [ -z "$input" ]; then
				input=$1
			else
				output=$1
			fi
			;;
	esac
	shift || break
done
test "$format" = binary
test -n "$input"
test -n "$output"
mkdir -p "$(dirname "$output")"
cp "$input" "$output"
EOF
chmod +x "$tmp/stagepkg/tools/fake-clang.sh" "$tmp/stagepkg/tools/fake-link.sh" "$tmp/stagepkg/tools/fake-objcopy.sh"
cat > "$tmp/stagepkg/src/efi_main.c" <<'EOF'
int efi_main(void *image, void *system_table) {
	(void)image;
	(void)system_table;
	return 0;
}
EOF
cat > "$tmp/stagepkg/fixtures/kernel.elf" <<'EOF'
ELF-RPI
payload
EOF
cat > "$tmp/stagepkg/boot/config.txt" <<'EOF'
kernel=kernel8.img
EOF
cat > "$tmp/stagepkg/boot/payload.bin" <<'EOF'
payload
EOF
cat > "$tmp/stagepkg/qstar.lua" <<'EOF'
qstar.profile "default" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "tools/fake-clang.sh",
  linker = "tools/fake-link.sh",
  response_style = "msvc",
  tool_overrides = {"llvm-objcopy=tools/fake-objcopy.sh"},
  artifact_names = {"//:boot=BOOTX64.EFI"},
}

qstar.executable "boot" {
  sources = {"src/efi_main.c"},
  link_options = {
    "/subsystem:efi_application",
    "/entry:efi_main",
    "/nodefaultlib",
  },
}

qstar.custom_target "kernel_img" {
  inputs = {"fixtures/kernel.elf"},
  outputs = {
    qstar.output("generated/kernel8.img", {
      group = "images",
      format = "raw-binary",
      address = "0x80000",
      layout = "rpi5-kernel8",
    }),
  },
  command = qstar.cli {
    "llvm-objcopy",
    "-O",
    "binary",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.stage "esp" {
  root = "stage/esp",
  files = {
    qstar.stage_file(qstar.target_file("//:boot"), "EFI/BOOT/BOOTX64.EFI"),
  },
}

qstar.stage "rpi" {
  root = "stage/rpi",
  files = {
    qstar.stage_file("boot/config.txt", "config.txt"),
    qstar.stage_file(qstar.target_file("//:kernel_img"), "kernel8.img"),
    qstar.stage_file("boot/payload.bin", "payload.bin"),
  },
}
EOF
if ! "$qstar" --file "$tmp/stagepkg/qstar.lua" check //:boot > "$tmp/stagepkg-check.out" 2> "$tmp/stagepkg-check.err"; then
	cat "$tmp/stagepkg-check.out" >&2
	cat "$tmp/stagepkg-check.err" >&2
	fail "stage package check failed"
fi
contains "$tmp/stagepkg-check.out" "stage-count 2"
if ! "$qstar" --file "$tmp/stagepkg/qstar.lua" list-targets --format json > "$tmp/stagepkg-targets-json.out" 2> "$tmp/stagepkg-targets-json.err"; then
	cat "$tmp/stagepkg-targets-json.out" >&2
	cat "$tmp/stagepkg-targets-json.err" >&2
	fail "stage package list-targets failed"
fi
contains "$tmp/stagepkg-targets-json.out" "\"stage_count\":2"
contains "$tmp/stagepkg-targets-json.out" "\"label\":\"//:esp\""
contains "$tmp/stagepkg-targets-json.out" "\"root\":\"stage/esp\""
if ! "$qstar" --file "$tmp/stagepkg/qstar.lua" stage //:esp --dry-run > "$tmp/stagepkg-esp-dry.out" 2> "$tmp/stagepkg-esp-dry.err"; then
	cat "$tmp/stagepkg-esp-dry.out" >&2
	cat "$tmp/stagepkg-esp-dry.err" >&2
	fail "stage package esp dry-run failed"
fi
contains "$tmp/stagepkg-esp-dry.out" "qstar stage v2"
contains "$tmp/stagepkg-esp-dry.out" "mode dry-run"
contains "$tmp/stagepkg-esp-dry.out" "stage_layout label=//:esp root=stage/esp files=1 status=ok"
contains "$tmp/stagepkg-esp-dry.out" "stage_file src=build/qstar/out/___boot/BOOTX64.EFI dst=stage/esp/EFI/BOOT/BOOTX64.EFI mode=dry-run"
contains "$tmp/stagepkg-esp-dry.out" "stage_diff dst=stage/esp/EFI/BOOT/BOOTX64.EFI action=would-create"
contains "$tmp/stagepkg/build/qstar/stage/___esp/manifest.json" "\"schema\":\"qstar-stage-manifest-v2\""
contains "$tmp/stagepkg/build/qstar/stage/___esp/manifest.json" "\"layout\":{\"status\":\"ok\",\"file_count\":1}"
contains "$tmp/stagepkg/build/qstar/stage/___esp/manifest.json" "\"mode\":\"dry-run\""
contains "$tmp/stagepkg/build/qstar/stage/___esp/manifest.json" "\"kind\":\"target\""
contains "$tmp/stagepkg/build/qstar/stage/___esp/manifest.json" "\"producer\":\"//:boot\""
if [ -f "$tmp/stagepkg/stage/esp/EFI/BOOT/BOOTX64.EFI" ]; then
	fail "stage dry-run unexpectedly copied ESP artifact"
fi
if ! "$qstar" --file "$tmp/stagepkg/qstar.lua" stage //:esp > "$tmp/stagepkg-esp-stage.out" 2> "$tmp/stagepkg-esp-stage.err"; then
	cat "$tmp/stagepkg-esp-stage.out" >&2
	cat "$tmp/stagepkg-esp-stage.err" >&2
	fail "stage package esp copy failed"
fi
contains "$tmp/stagepkg-esp-stage.out" "stage_diff dst=stage/esp/EFI/BOOT/BOOTX64.EFI action=would-create"
contains "$tmp/stagepkg-esp-stage.out" "status ok"
test -f "$tmp/stagepkg/stage/esp/EFI/BOOT/BOOTX64.EFI" || fail "missing staged ESP BOOTX64.EFI"
contains "$tmp/stagepkg/build/qstar/stage/___esp/manifest.json" "\"mode\":\"copy\""
if ! "$qstar" --file "$tmp/stagepkg/qstar.lua" stage //:esp --dry-run > "$tmp/stagepkg-esp-dry2.out" 2> "$tmp/stagepkg-esp-dry2.err"; then
	cat "$tmp/stagepkg-esp-dry2.out" >&2
	cat "$tmp/stagepkg-esp-dry2.err" >&2
	fail "stage package esp second dry-run failed"
fi
contains "$tmp/stagepkg-esp-dry2.out" "stage_diff dst=stage/esp/EFI/BOOT/BOOTX64.EFI action=unchanged"
if ! "$qstar" --file "$tmp/stagepkg/qstar.lua" stage //:rpi > "$tmp/stagepkg-rpi-stage.out" 2> "$tmp/stagepkg-rpi-stage.err"; then
	cat "$tmp/stagepkg-rpi-stage.out" >&2
	cat "$tmp/stagepkg-rpi-stage.err" >&2
	fail "stage package rpi copy failed"
fi
contains "$tmp/stagepkg-rpi-stage.out" "stage_file src=boot/config.txt dst=stage/rpi/config.txt mode=copy"
contains "$tmp/stagepkg-rpi-stage.out" "stage_file src=generated/kernel8.img dst=stage/rpi/kernel8.img mode=copy"
contains "$tmp/stagepkg-rpi-stage.out" "stage_file src=boot/payload.bin dst=stage/rpi/payload.bin mode=copy"
test -f "$tmp/stagepkg/stage/rpi/config.txt" || fail "missing staged RPi config.txt"
test -f "$tmp/stagepkg/stage/rpi/kernel8.img" || fail "missing staged RPi kernel8.img"
test -f "$tmp/stagepkg/stage/rpi/payload.bin" || fail "missing staged RPi payload.bin"
cmp "$tmp/stagepkg/fixtures/kernel.elf" "$tmp/stagepkg/stage/rpi/kernel8.img" >/dev/null || fail "staged RPi image content drifted"
contains "$tmp/stagepkg/build/qstar/stage/___rpi/manifest.json" "\"label\":\"//:rpi\""
contains "$tmp/stagepkg/build/qstar/stage/___rpi/manifest.json" "\"dst\":\"stage/rpi/kernel8.img\""
contains "$tmp/stagepkg/build/qstar/stage/___rpi/manifest.json" "\"kind\":\"custom_target\""
contains "$tmp/stagepkg/build/qstar/stage/___rpi/manifest.json" "\"producer\":\"//:kernel_img\""
contains "$tmp/stagepkg/build/qstar/stage/___rpi/manifest.json" "\"kind\":\"file\""
if ! "$qstar" --file "$tmp/stagepkg/qstar.lua" stage //:esp --root stage/custom-esp --dry-run > "$tmp/stagepkg-esp-root.out" 2> "$tmp/stagepkg-esp-root.err"; then
	cat "$tmp/stagepkg-esp-root.out" >&2
	cat "$tmp/stagepkg-esp-root.err" >&2
	fail "stage package esp custom root failed"
fi
contains "$tmp/stagepkg-esp-root.out" "stage-root stage/custom-esp"
contains "$tmp/stagepkg-esp-root.out" "dst=stage/custom-esp/EFI/BOOT/BOOTX64.EFI"

step "stage diagnostics"
mkdir -p "$tmp/stage-bad/src"
cat > "$tmp/stage-bad/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/stage-bad/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
}

qstar.stage "bad" {
  root = "../stage",
  files = {
    qstar.stage_file(qstar.target_file("//:app"), "app.bin"),
  },
}
EOF
if "$qstar" --file "$tmp/stage-bad/qstar.lua" check > "$tmp/stage-bad-root.out" 2> "$tmp/stage-bad-root.err"; then
	fail "stage root escape unexpectedly succeeded"
fi
contains "$tmp/stage-bad-root.err" "stage root '../stage' in '//:bad' must be package-relative"
cat > "$tmp/stage-bad/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
}

qstar.stage "bad" {
  root = "stage/bad",
  files = {
    qstar.stage_file(qstar.target_file("//:app"), "../app.bin"),
  },
}
EOF
if "$qstar" --file "$tmp/stage-bad/qstar.lua" check > "$tmp/stage-bad-dst.out" 2> "$tmp/stage-bad-dst.err"; then
	fail "stage destination escape unexpectedly succeeded"
fi
contains "$tmp/stage-bad-dst.err" "stage destination '../app.bin' in '//:bad' must be package-relative"
cat > "$tmp/stage-bad/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
}

qstar.stage "bad" {
  root = "stage/bad",
  files = {
    qstar.stage_file(qstar.target_file("//:app"), "app.bin"),
    qstar.stage_file("src/main.c", "app.bin"),
  },
}
EOF
if "$qstar" --file "$tmp/stage-bad/qstar.lua" check > "$tmp/stage-bad-dup.out" 2> "$tmp/stage-bad-dup.err"; then
	fail "duplicate stage destination unexpectedly succeeded"
fi
contains "$tmp/stage-bad-dup.err" "stage destination 'app.bin' in '//:bad' is duplicated"
cat > "$tmp/stage-bad/qstar.lua" <<'EOF'
qstar.stage "bad" {
  root = "stage/bad",
  files = {
    qstar.stage_file("src/main.c", "EFI/BOOT"),
    qstar.stage_file("src/main.c", "EFI/BOOT/BOOTX64.EFI"),
  },
}
EOF
if "$qstar" --file "$tmp/stage-bad/qstar.lua" check > "$tmp/stage-bad-layout.out" 2> "$tmp/stage-bad-layout.err"; then
	fail "stage destination layout conflict unexpectedly succeeded"
fi
contains "$tmp/stage-bad-layout.err" "stage destination layout conflict 'EFI/BOOT' and 'EFI/BOOT/BOOTX64.EFI' in '//:bad'"
cat > "$tmp/stage-bad/qstar.lua" <<'EOF'
qstar.stage "bad" {
  root = "stage/bad",
  files = {
    qstar.stage_file(qstar.target_file("//:missing"), "missing.bin"),
  },
}
EOF
if "$qstar" --file "$tmp/stage-bad/qstar.lua" check > "$tmp/stage-bad-missing.out" 2> "$tmp/stage-bad-missing.err"; then
	fail "unknown stage target_file unexpectedly succeeded"
fi
contains "$tmp/stage-bad-missing.err" "stage source target '//:missing' in '//:bad' is unknown"

echo "qstar-smoke: passed"
