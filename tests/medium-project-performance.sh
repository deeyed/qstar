#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-medium-project-performance.$$
root=$tmp/project
daemon_dir=${QSTAR_DAEMON_TMPDIR:-/tmp}/qstar-medium-daemon.$$
clean_max_ms=${QSTAR_MEDIUM_CLEAN_MAX_MS:-120000}
noop_max_ms=${QSTAR_MEDIUM_NOOP_MAX_MS:-300}
incremental_max_ms=${QSTAR_MEDIUM_INCREMENTAL_MAX_MS:-1000}
ratio_x100=${QSTAR_MEDIUM_STELLA_TO_NINJA_X100:-200}
ratio_slack_ms=${QSTAR_MEDIUM_RATIO_SLACK_MS:-250}
min_targets=${QSTAR_MEDIUM_MIN_TARGETS:-40}
report_only=${QSTAR_MEDIUM_PERF_REPORT_ONLY:-1}
perf_issue_count=0
daemon_pid=

fail() {
	echo "qstar-medium-project: $*" >&2
	exit 1
}

contains() {
	file=$1
	pat=$2
	grep -F -q -- "$pat" "$file" || fail "missing pattern '$pat' in $file"
}

not_contains() {
	file=$1
	pat=$2
	if grep -F -q -- "$pat" "$file"; then
		fail "unexpected pattern '$pat' in $file"
	fi
}

now_ms() {
	if command -v python3 >/dev/null 2>&1; then
		python3 -c 'import time; print(int(time.time() * 1000))'
	else
		printf '%s000\n' "$(date +%s)"
	fi
}

detect_host_jobs() {
	jobs=
	if command -v getconf >/dev/null 2>&1; then
		jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
	fi
	if [ -z "$jobs" ] && command -v sysctl >/dev/null 2>&1; then
		jobs=$(sysctl -n hw.ncpu 2>/dev/null || true)
	fi
	case "$jobs" in
	''|*[!0-9]*)
		jobs=1
		;;
	esac
	if [ "$jobs" -lt 1 ]; then
		jobs=1
	fi
	printf '%s\n' "$jobs"
}

field_value() {
	line=$1
	name=$2
	printf '%s\n' "$line" | sed -n "s/.*${name}=\\([^ ]*\\).*/\\1/p"
}

run_timed() {
	name=$1
	shift
	start=$(now_ms)
	if ! "$@" > "$tmp/$name.out" 2> "$tmp/$name.err"; then
		cat "$tmp/$name.out" >&2
		cat "$tmp/$name.err" >&2
		return 1
	fi
	end=$(now_ms)
	eval "${name}_ms=\$((end - start))"
}

check_elapsed_max() {
	name=$1
	elapsed=$2
	limit=$3
	if [ "$elapsed" -gt "$limit" ]; then
		perf_issue "$name took ${elapsed}ms; limit=${limit}ms"
	fi
}

check_stella_vs_ninja() {
	phase=$1
	stella_elapsed=$2
	ninja_elapsed=$3
	if [ $((stella_elapsed * 100)) -gt $((ninja_elapsed * ratio_x100 + ratio_slack_ms * 100)) ]; then
		perf_issue "stella $phase ${stella_elapsed}ms exceeds ninja ${ninja_elapsed}ms beyond ratio_x100=${ratio_x100} slack_ms=${ratio_slack_ms}"
	fi
}

perf_issue() {
	perf_issue_count=$((perf_issue_count + 1))
	if [ "$report_only" = 1 ]; then
		printf 'medium_project_gate warning=%s\n' "$*"
	else
		fail "$*"
	fi
}

bump_source() {
	path=$1
	value=$2
	sleep 1
	printf 'int module_cache(void) { return %s; }\n' "$value" > "$root/$path"
}

write_staticlib() {
	dir=$1
	target=$2
	func=$3
	value=$4
	deps=$5
	base=${dir##*/}
	mkdir -p "$root/$dir"
	{
		printf 'qstar.staticlib "%s" {\n' "$target"
		printf '  configs = {"//:module_c"},\n'
		printf '  sources = {"%s/%s.c"},\n' "$dir" "$base"
		if [ -n "$deps" ]; then
			printf '  deps = {%s},\n' "$deps"
		fi
		printf '}\n'
	} > "$root/$dir/$base.qst"
	printf 'int %s(void) { return %s; }\n' "$func" "$value" > "$root/$dir/$base.c"
}

cleanup() {
	if [ -n "${daemon_pid:-}" ]; then
		kill "$daemon_pid" 2>/dev/null || true
		wait "$daemon_pid" 2>/dev/null || true
	fi
	rm -rf "$tmp" "$daemon_dir"
}

rm -rf "$tmp" "$daemon_dir"
mkdir -p "$root"
trap cleanup EXIT HUP INT TERM

cat > "$root/qstar.lua" <<'EOF'
qstar.project {
  name = "medium-package-corpus",
  root = ".",
  build_dir = "build/qstar",
  compile_commands = "build",
}

qstar.config "module_c" {
  lang = {
    c = {
      compile_options = {
        "-std=c99",
        "-Wall",
        "-Wextra",
      },
    },
  },
}

qstar.subdir("lib/base")
qstar.subdir("modules/variant")
qstar.subdir("modules/product")
qstar.subdir("modules/core")
qstar.subdir("plugins/io")
qstar.subdir("runtime/env")
qstar.subdir("services/network")

qstar.group "package_bundle" {
  deps = {
    "//lib/base:base_core",
    "//modules/variant:variant_modules",
    "//modules/product:product_modules",
    "//modules/core:core_modules",
    "//plugins/io:plugin_stack",
    "//runtime/env:runtime_stack",
    "//services/network:service_stack",
  },
}
EOF

cat > "$root/sys_arch.qst.tmp" <<'EOF'
qstar.subdir("alpha")
qstar.subdir("beta")

qstar.group "variant_modules" {
  deps = {
    "//modules/variant/alpha:variant_alpha",
    "//modules/variant/beta:variant_beta",
  },
}
EOF
mkdir -p "$root/modules/variant"
mv "$root/sys_arch.qst.tmp" "$root/modules/variant/variant.qst"

cat > "$root/sys_board.qst.tmp" <<'EOF'
qstar.subdir("line/a")
qstar.subdir("line/b")

qstar.group "product_modules" {
  deps = {
    "//modules/product/line/a:product_alpha",
    "//modules/product/line/b:product_beta",
  },
}
EOF
mkdir -p "$root/modules/product"
mv "$root/sys_board.qst.tmp" "$root/modules/product/product.qst"

cat > "$root/sys_kern.qst.tmp" <<'EOF'
qstar.subdir("start")
qstar.subdir("cache")
qstar.subdir("signal")
qstar.subdir("time")
qstar.subdir("runner")
qstar.subdir("worker")
qstar.subdir("adapter")
qstar.subdir("store")
qstar.subdir("gateway")
qstar.subdir("model")
qstar.subdir("lock")

qstar.group "core_modules" {
  deps = {
    "//modules/core/start:module_start",
    "//modules/core/cache:module_cache",
    "//modules/core/signal:module_signal",
    "//modules/core/time:module_clock",
    "//modules/core/runner:module_runner",
    "//modules/core/worker:module_worker",
    "//modules/core/adapter:module_adapter",
    "//modules/core/store:module_store",
    "//modules/core/gateway:module_gateway",
    "//modules/core/model:module_model",
    "//modules/core/lock:module_lock",
  },
}
EOF
mkdir -p "$root/modules/core"
mv "$root/sys_kern.qst.tmp" "$root/modules/core/core.qst"

cat > "$root/plugins_io.qst.tmp" <<'EOF'
qstar.subdir("stream")
qstar.subdir("timer")
qstar.subdir("input")
qstar.subdir("output")
qstar.subdir("bus-a")
qstar.subdir("bus-b")
qstar.subdir("store")
qstar.subdir("monitor")
qstar.subdir("clock")
qstar.subdir("transfer")
qstar.subdir("message")
qstar.subdir("random")

qstar.group "plugin_stack" {
  deps = {
    "//plugins/io/stream:plugin_stream",
    "//plugins/io/timer:plugin_timer",
    "//plugins/io/input:plugin_input",
    "//plugins/io/output:plugin_output",
    "//plugins/io/bus-a:plugin_bus_a",
    "//plugins/io/bus-b:plugin_bus_b",
    "//plugins/io/store:plugin_store",
    "//plugins/io/monitor:plugin_monitor",
    "//plugins/io/clock:plugin_clock",
    "//plugins/io/transfer:plugin_transfer",
    "//plugins/io/message:plugin_message",
    "//plugins/io/random:plugin_random",
  },
}
EOF
mkdir -p "$root/plugins/io"
mv "$root/plugins_io.qst.tmp" "$root/plugins/io/io.qst"

cat > "$root/sys_platform.qst.tmp" <<'EOF'
qstar.subdir("clock")
qstar.subdir("power")
qstar.subdir("memory")
qstar.subdir("signal")
qstar.subdir("flow")
qstar.subdir("manifest")

qstar.group "runtime_stack" {
  deps = {
    "//runtime/env/clock:runtime_clock",
    "//runtime/env/power:runtime_power",
    "//runtime/env/memory:runtime_memory",
    "//runtime/env/signal:runtime_signal",
    "//runtime/env/flow:runtime_flow",
    "//runtime/env/manifest:runtime_manifest",
  },
}
EOF
mkdir -p "$root/runtime/env"
mv "$root/sys_platform.qst.tmp" "$root/runtime/env/env.qst"

cat > "$root/sys_net.qst.tmp" <<'EOF'
qstar.subdir("link")
qstar.subdir("route")
qstar.subdir("packet")
qstar.subdir("discovery")
qstar.subdir("console")
qstar.subdir("telemetry")

qstar.group "service_stack" {
  deps = {
    "//services/network/link:service_link",
    "//services/network/route:service_route",
    "//services/network/packet:service_packet",
    "//services/network/discovery:service_discovery",
    "//services/network/console:service_console",
    "//services/network/telemetry:service_telemetry",
  },
}
EOF
mkdir -p "$root/services/network"
mv "$root/sys_net.qst.tmp" "$root/services/network/network.qst"

write_staticlib "lib/base" "base_core" "base_core" 1 ""
write_staticlib "modules/variant/alpha" "variant_alpha" "variant_alpha" 2 '"//lib/base:base_core"'
write_staticlib "modules/variant/beta" "variant_beta" "variant_beta" 3 '"//lib/base:base_core"'
write_staticlib "modules/product/line/a" "product_alpha" "product_alpha" 4 '"//lib/base:base_core"'
write_staticlib "modules/product/line/b" "product_beta" "product_beta" 5 '"//lib/base:base_core"'
write_staticlib "modules/core/start" "module_start" "module_start" 6 '"//lib/base:base_core"'
write_staticlib "modules/core/cache" "module_cache" "module_cache" 7 '"//lib/base:base_core"'
write_staticlib "modules/core/signal" "module_signal" "module_signal" 8 '"//lib/base:base_core"'
write_staticlib "modules/core/time" "module_clock" "module_clock" 9 '"//lib/base:base_core"'
write_staticlib "modules/core/runner" "module_runner" "module_runner" 10 '"//lib/base:base_core"'
write_staticlib "modules/core/worker" "module_worker" "module_worker" 11 '"//lib/base:base_core"'
write_staticlib "modules/core/adapter" "module_adapter" "module_adapter" 12 '"//lib/base:base_core"'
write_staticlib "modules/core/store" "module_store" "module_store" 13 '"//lib/base:base_core"'
write_staticlib "modules/core/gateway" "module_gateway" "module_gateway" 14 '"//lib/base:base_core"'
write_staticlib "modules/core/model" "module_model" "module_model" 15 '"//lib/base:base_core"'
write_staticlib "modules/core/lock" "module_lock" "module_lock" 16 '"//lib/base:base_core"'
write_staticlib "plugins/io/stream" "plugin_stream" "plugin_stream" 17 '"//lib/base:base_core"'
write_staticlib "plugins/io/timer" "plugin_timer" "plugin_timer" 18 '"//lib/base:base_core"'
write_staticlib "plugins/io/input" "plugin_input" "plugin_input" 19 '"//lib/base:base_core"'
write_staticlib "plugins/io/output" "plugin_output" "plugin_output" 20 '"//lib/base:base_core"'
write_staticlib "plugins/io/bus-a" "plugin_bus_a" "plugin_bus_a" 21 '"//lib/base:base_core"'
write_staticlib "plugins/io/bus-b" "plugin_bus_b" "plugin_bus_b" 22 '"//lib/base:base_core"'
write_staticlib "plugins/io/store" "plugin_store" "plugin_store" 23 '"//lib/base:base_core"'
write_staticlib "plugins/io/monitor" "plugin_monitor" "plugin_monitor" 24 '"//lib/base:base_core"'
write_staticlib "plugins/io/clock" "plugin_clock" "plugin_clock" 25 '"//lib/base:base_core"'
write_staticlib "plugins/io/transfer" "plugin_transfer" "plugin_transfer" 26 '"//lib/base:base_core"'
write_staticlib "plugins/io/message" "plugin_message" "plugin_message" 27 '"//lib/base:base_core"'
write_staticlib "plugins/io/random" "plugin_random" "plugin_random" 28 '"//lib/base:base_core"'
write_staticlib "runtime/env/clock" "runtime_clock" "runtime_clock" 29 '"//lib/base:base_core"'
write_staticlib "runtime/env/power" "runtime_power" "runtime_power" 30 '"//lib/base:base_core"'
write_staticlib "runtime/env/memory" "runtime_memory" "runtime_memory" 31 '"//lib/base:base_core"'
write_staticlib "runtime/env/signal" "runtime_signal" "runtime_signal" 32 '"//lib/base:base_core"'
write_staticlib "runtime/env/flow" "runtime_flow" "runtime_flow" 33 '"//lib/base:base_core"'
write_staticlib "runtime/env/manifest" "runtime_manifest" "runtime_manifest" 34 '"//lib/base:base_core"'
write_staticlib "services/network/link" "service_link" "service_link" 35 '"//lib/base:base_core"'
write_staticlib "services/network/route" "service_route" "service_route" 36 '"//lib/base:base_core"'
write_staticlib "services/network/packet" "service_packet" "service_packet" 37 '"//lib/base:base_core"'
write_staticlib "services/network/discovery" "service_discovery" "service_discovery" 38 '"//lib/base:base_core"'
write_staticlib "services/network/console" "service_console" "service_console" 39 '"//lib/base:base_core"'
write_staticlib "services/network/telemetry" "service_telemetry" "service_telemetry" 40 '"//lib/base:base_core"'

"$qstar" --file "$root/qstar.lua" check //:package_bundle > "$tmp/check.out" 2> "$tmp/check.err"
contains "$tmp/check.out" "status ok"
target_count=$(awk '/^target-count / {print $2}' "$tmp/check.out")
if [ -z "$target_count" ] || [ "$target_count" -lt "$min_targets" ]; then
	fail "medium corpus target-count=${target_count:-missing}; minimum=${min_targets}"
fi
"$qstar" --file "$root/qstar.lua" lint //... > "$tmp/lint.out" 2> "$tmp/lint.err"
contains "$tmp/lint.out" "status ok"
printf 'medium_project_gate target_count=%s min_targets=%s\n' "$target_count" "$min_targets"

host_jobs=$(detect_host_jobs)
printf 'medium_project_gate scheduler host_jobs=%s\n' "$host_jobs"

run_timed stella_trace "$qstar" --file "$root/qstar.lua" -B build/stella-trace -G stella build //:package_bundle --schedule-trace --progress off --color never
contains "$tmp/stella_trace.out" "executor-policy version=v4"
contains "$tmp/stella_trace.out" "action_scheduler version=v1"
contains "$tmp/stella_trace.out" "status ok"
not_contains "$tmp/stella_trace.out" "parallel=no jobs=1 active=serial-ready-queue"
not_contains "$tmp/stella_trace.out" "kind=final state=ready"
policy_line=$(grep -m 1 '^executor-policy ' "$tmp/stella_trace.out")
scheduler_line=$(grep -m 1 '^action_scheduler ' "$tmp/stella_trace.out")
default_jobs=$(field_value "$policy_line" jobs)
initial_ready=$(field_value "$scheduler_line" ready)
async_final_count=$(grep -E 'schedule_action id=.*:(archive|link):0 kind=(archive|link) slot=' "$tmp/stella_trace.out" | wc -l | tr -d ' ')
runner=$(sed -n 's/.* runner=\([^ ]*\) .*/\1/p' "$tmp/stella_trace.out" | head -n 1)
case "$(uname -s)" in
Darwin|Linux)
	if [ "$runner" != "posix_spawn" ]; then
		fail "expected posix_spawn runner on POSIX host, got ${runner:-missing}"
	fi
	event_wait=poll
	;;
*)
	if [ -z "$runner" ]; then
		fail "could not parse scheduler runner"
	fi
	event_wait=platform
	;;
esac
if [ -z "$default_jobs" ] || [ "$default_jobs" -lt 1 ]; then
	fail "could not parse default scheduler jobs"
fi
if [ -z "$initial_ready" ] || [ "$initial_ready" -lt 2 ]; then
	fail "ready queue width too narrow: ${initial_ready:-missing}"
fi
if [ "$host_jobs" -gt 1 ] && [ "$default_jobs" -le 1 ]; then
	fail "default jobs fell back to serial on multi-core host"
fi
if [ "$async_final_count" -lt 2 ]; then
	fail "expected async archive/link final actions in schedule trace"
fi
printf 'medium_project_gate scheduler default_jobs=%s ready_width=%s async_final_actions=%s trace_elapsed_ms=%s\n' "$default_jobs" "$initial_ready" "$async_final_count" "$stella_trace_ms"
printf 'medium_project_gate scheduler runner=%s event_wait=%s\n' "$runner" "$event_wait"

run_timed stella_clean "$qstar" --file "$root/qstar.lua" -B build/stella -G stella build //:package_bundle
contains "$tmp/stella_clean.out" "group_target label=//:package_bundle"
contains "$tmp/stella_clean.out" "status ok"
test -f "$root/build/stella/compile_commands.json" || fail "stella compile_commands missing"
check_elapsed_max "stella clean build" "$stella_clean_ms" "$clean_max_ms"
printf 'medium_project_gate backend=stella phase=clean elapsed_ms=%s\n' "$stella_clean_ms"

run_timed stella_noop "$qstar" --file "$root/qstar.lua" -B build/stella -G stella build //:package_bundle
contains "$tmp/stella_noop.out" "status ok"
check_elapsed_max "stella no-op build" "$stella_noop_ms" "$noop_max_ms"
printf 'medium_project_gate backend=stella phase=noop elapsed_ms=%s\n' "$stella_noop_ms"

bump_source "modules/core/cache/cache.c" 7001
run_timed stella_incremental "$qstar" --file "$root/qstar.lua" -B build/stella -G stella build //:package_bundle
contains "$tmp/stella_incremental.out" "status ok"
check_elapsed_max "stella incremental build" "$stella_incremental_ms" "$incremental_max_ms"
printf 'medium_project_gate backend=stella phase=incremental elapsed_ms=%s\n' "$stella_incremental_ms"

mkdir -p "$daemon_dir"
chmod 700 "$daemon_dir"
daemon_sock="$daemon_dir/qstar-medium-daemon.sock"
rm -f "$daemon_sock"
"$qstar" --file "$root/qstar.lua" -B build/stella-daemon daemon --socket "$daemon_sock" --serve > "$tmp/stella_daemon_server.out" 2> "$tmp/stella_daemon_server.err" &
daemon_pid=$!
i=0
while [ ! -S "$daemon_sock" ] && kill -0 "$daemon_pid" 2>/dev/null && [ "$i" -lt 30 ]; do
	sleep 0.1
	i=$((i + 1))
done
if [ -S "$daemon_sock" ]; then
	run_timed stella_daemon_clean "$qstar" --file "$root/qstar.lua" -B build/stella-daemon -G stella build //:package_bundle --use-daemon=always --daemon-socket "$daemon_sock" --progress off --color never
	contains "$tmp/stella_daemon_clean.out" "status ok"
	test -f "$root/build/stella-daemon/compile_commands.json" || fail "stella daemon compile_commands missing"
	run_timed stella_daemon_noop "$qstar" --file "$root/qstar.lua" -B build/stella-daemon -G stella build //:package_bundle --use-daemon=always --daemon-socket "$daemon_sock" --progress off --color never
	contains "$tmp/stella_daemon_noop.out" "status ok"
	bump_source "modules/core/cache/cache.c" 7004
	run_timed stella_daemon_incremental "$qstar" --file "$root/qstar.lua" -B build/stella-daemon -G stella build //:package_bundle --use-daemon=always --daemon-socket "$daemon_sock" --progress off --color never
	contains "$tmp/stella_daemon_incremental.out" "status ok"
	printf 'medium_project_gate backend=stella-daemon phase=clean elapsed_ms=%s cli_clean_ms=%s\n' "$stella_daemon_clean_ms" "$stella_clean_ms"
	printf 'medium_project_gate backend=stella-daemon phase=noop elapsed_ms=%s cli_noop_ms=%s\n' "$stella_daemon_noop_ms" "$stella_noop_ms"
	printf 'medium_project_gate backend=stella-daemon phase=incremental elapsed_ms=%s cli_incremental_ms=%s\n' "$stella_daemon_incremental_ms" "$stella_incremental_ms"
	kill "$daemon_pid" 2>/dev/null || true
	wait "$daemon_pid" 2>/dev/null || true
	daemon_pid=
	rm -f "$daemon_sock"
else
	if grep -E -q "Operation not permitted|Permission denied|permission denied" "$tmp/stella_daemon_server.err"; then
		printf 'medium_project_gate backend=stella-daemon phase=clean elapsed_ms=skipped reason=socket-bind-not-permitted\n'
		printf 'medium_project_gate backend=stella-daemon phase=noop elapsed_ms=skipped reason=socket-bind-not-permitted\n'
		printf 'medium_project_gate backend=stella-daemon phase=incremental elapsed_ms=skipped reason=socket-bind-not-permitted\n'
		kill "$daemon_pid" 2>/dev/null || true
		wait "$daemon_pid" 2>/dev/null || true
		daemon_pid=
	else
		cat "$tmp/stella_daemon_server.err" >&2
		fail "experimental daemon socket did not become ready"
	fi
fi

run_timed stella_jobs_clean "$qstar" --file "$root/qstar.lua" -B build/stella-jobs -G stella build //:package_bundle --jobs "$host_jobs"
contains "$tmp/stella_jobs_clean.out" "group_target label=//:package_bundle"
contains "$tmp/stella_jobs_clean.out" "status ok"
test -f "$root/build/stella-jobs/compile_commands.json" || fail "stella --jobs compile_commands missing"
check_elapsed_max "stella --jobs clean build" "$stella_jobs_clean_ms" "$clean_max_ms"
printf 'medium_project_gate backend=stella-jobs jobs=%s phase=clean elapsed_ms=%s\n' "$host_jobs" "$stella_jobs_clean_ms"

run_timed stella_jobs_noop "$qstar" --file "$root/qstar.lua" -B build/stella-jobs -G stella build //:package_bundle --jobs "$host_jobs"
contains "$tmp/stella_jobs_noop.out" "status ok"
check_elapsed_max "stella --jobs no-op build" "$stella_jobs_noop_ms" "$noop_max_ms"
printf 'medium_project_gate backend=stella-jobs jobs=%s phase=noop elapsed_ms=%s\n' "$host_jobs" "$stella_jobs_noop_ms"

bump_source "modules/core/cache/cache.c" 7002
run_timed stella_jobs_incremental "$qstar" --file "$root/qstar.lua" -B build/stella-jobs -G stella build //:package_bundle --jobs "$host_jobs"
contains "$tmp/stella_jobs_incremental.out" "status ok"
check_elapsed_max "stella --jobs incremental build" "$stella_jobs_incremental_ms" "$incremental_max_ms"
printf 'medium_project_gate backend=stella-jobs jobs=%s phase=incremental elapsed_ms=%s\n' "$host_jobs" "$stella_jobs_incremental_ms"

"$qstar" --file "$root/qstar.lua" -B build/stella action-log //modules/core/cache:module_cache:archive:0 > "$tmp/module-cache-archive-log.out" 2> "$tmp/module-cache-archive-log.err"
contains "$tmp/module-cache-archive-log.out" "argv[0]=ar"
contains "$tmp/module-cache-archive-log.out" "libmodule_cache.a"
not_contains "$tmp/module-cache-archive-log.out" "libbase_core.a"
printf 'medium_project_gate staticlib_argv_parity=ok target=//modules/core/cache:module_cache\n'

if command -v ninja >/dev/null 2>&1; then
	run_timed ninja_clean "$qstar" --file "$root/qstar.lua" -B build/qstar-ninja -G ninja build //:package_bundle
	contains "$tmp/ninja_clean.out" "backend ninja"
	contains "$tmp/ninja_clean.out" "status ok"
	test -f "$root/build/qstar-ninja/compile_commands.json" || fail "ninja compile_commands missing"
	check_elapsed_max "ninja clean build" "$ninja_clean_ms" "$clean_max_ms"
	printf 'medium_project_gate backend=ninja phase=clean elapsed_ms=%s\n' "$ninja_clean_ms"
	run_timed ninja_noop "$qstar" --file "$root/qstar.lua" -B build/qstar-ninja -G ninja build //:package_bundle
	contains "$tmp/ninja_noop.out" "backend ninja"
	contains "$tmp/ninja_noop.out" "status ok"
	check_elapsed_max "ninja no-op build" "$ninja_noop_ms" "$noop_max_ms"
	printf 'medium_project_gate backend=ninja phase=noop elapsed_ms=%s\n' "$ninja_noop_ms"
	bump_source "modules/core/cache/cache.c" 7003
	run_timed ninja_incremental "$qstar" --file "$root/qstar.lua" -B build/qstar-ninja -G ninja build //:package_bundle
	contains "$tmp/ninja_incremental.out" "backend ninja"
	contains "$tmp/ninja_incremental.out" "status ok"
	check_elapsed_max "ninja incremental build" "$ninja_incremental_ms" "$incremental_max_ms"
	printf 'medium_project_gate backend=ninja phase=incremental elapsed_ms=%s\n' "$ninja_incremental_ms"
	printf 'medium_project_gate compare phase=clean stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$stella_clean_ms" "$ninja_clean_ms" "$ratio_x100" "$ratio_slack_ms"
	printf 'medium_project_gate compare phase=noop stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$stella_noop_ms" "$ninja_noop_ms" "$ratio_x100" "$ratio_slack_ms"
	printf 'medium_project_gate compare phase=incremental stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$stella_incremental_ms" "$ninja_incremental_ms" "$ratio_x100" "$ratio_slack_ms"
	printf 'medium_project_gate compare backend=stella-jobs phase=clean stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$stella_jobs_clean_ms" "$ninja_clean_ms" "$ratio_x100" "$ratio_slack_ms"
	printf 'medium_project_gate compare backend=stella-jobs phase=noop stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$stella_jobs_noop_ms" "$ninja_noop_ms" "$ratio_x100" "$ratio_slack_ms"
	printf 'medium_project_gate compare backend=stella-jobs phase=incremental stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$stella_jobs_incremental_ms" "$ninja_incremental_ms" "$ratio_x100" "$ratio_slack_ms"
	if [ -n "${stella_daemon_clean_ms:-}" ]; then
		printf 'medium_project_gate compare backend=stella-daemon phase=clean stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$stella_daemon_clean_ms" "$ninja_clean_ms" "$ratio_x100" "$ratio_slack_ms"
		printf 'medium_project_gate compare backend=stella-daemon phase=noop stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$stella_daemon_noop_ms" "$ninja_noop_ms" "$ratio_x100" "$ratio_slack_ms"
		printf 'medium_project_gate compare backend=stella-daemon phase=incremental stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$stella_daemon_incremental_ms" "$ninja_incremental_ms" "$ratio_x100" "$ratio_slack_ms"
	fi
	check_stella_vs_ninja clean "$stella_clean_ms" "$ninja_clean_ms"
	check_stella_vs_ninja noop "$stella_noop_ms" "$ninja_noop_ms"
	check_stella_vs_ninja incremental "$stella_incremental_ms" "$ninja_incremental_ms"
	check_stella_vs_ninja clean "$stella_jobs_clean_ms" "$ninja_clean_ms"
	check_stella_vs_ninja noop "$stella_jobs_noop_ms" "$ninja_noop_ms"
	check_stella_vs_ninja incremental "$stella_jobs_incremental_ms" "$ninja_incremental_ms"
	if [ -n "${stella_daemon_clean_ms:-}" ]; then
		check_stella_vs_ninja clean "$stella_daemon_clean_ms" "$ninja_clean_ms"
		check_stella_vs_ninja noop "$stella_daemon_noop_ms" "$ninja_noop_ms"
		check_stella_vs_ninja incremental "$stella_daemon_incremental_ms" "$ninja_incremental_ms"
	fi
else
	printf 'medium_project_gate backend=ninja phase=clean elapsed_ms=skipped reason=ninja-not-found\n'
	printf 'medium_project_gate backend=ninja phase=noop elapsed_ms=skipped reason=ninja-not-found\n'
	printf 'medium_project_gate backend=ninja phase=incremental elapsed_ms=skipped reason=ninja-not-found\n'
fi

printf 'medium_project_gate status=ok perf_issue_count=%s report_only=%s\n' "$perf_issue_count" "$report_only"
