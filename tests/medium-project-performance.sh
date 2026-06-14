#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-medium-project-performance.$$
root=$tmp/project
clean_max_ms=${QSTAR_MEDIUM_CLEAN_MAX_MS:-120000}
noop_max_ms=${QSTAR_MEDIUM_NOOP_MAX_MS:-300}
incremental_max_ms=${QSTAR_MEDIUM_INCREMENTAL_MAX_MS:-1000}
ratio_x100=${QSTAR_MEDIUM_STELLA_TO_NINJA_X100:-200}
ratio_slack_ms=${QSTAR_MEDIUM_RATIO_SLACK_MS:-250}
min_targets=${QSTAR_MEDIUM_MIN_TARGETS:-40}
report_only=${QSTAR_MEDIUM_PERF_REPORT_ONLY:-1}
perf_issue_count=0

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
	printf 'int kernel_mm(void) { return %s; }\n' "$value" > "$root/$path"
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
		printf '  configs = {"//:low_level_c"},\n'
		printf '  sources = {"%s/%s.c"},\n' "$dir" "$base"
		if [ -n "$deps" ]; then
			printf '  deps = {%s},\n' "$deps"
		fi
		printf '}\n'
	} > "$root/$dir/$base.qst"
	printf 'int %s(void) { return %s; }\n' "$func" "$value" > "$root/$dir/$base.c"
}

rm -rf "$tmp"
mkdir -p "$root"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cat > "$root/qstar.lua" <<'EOF'
qstar.project {
  name = "medium-firmware-corpus",
  root = ".",
  build_dir = "build/qstar",
  compile_commands = "build",
}

qstar.config "low_level_c" {
  lang = {
    c = {
      compile_options = {
        "-std=c99",
        "-ffreestanding",
        "-fno-builtin",
        "-Wall",
        "-Wextra",
      },
    },
  },
}

qstar.subdir("lib/libk")
qstar.subdir("sys/arch")
qstar.subdir("sys/board")
qstar.subdir("sys/kern")
qstar.subdir("sys/dev")
qstar.subdir("sys/platform")
qstar.subdir("sys/net")

qstar.group "firmware_image" {
  deps = {
    "//lib/libk:libk_core",
    "//sys/arch:arch_variants",
    "//sys/board:board_variants",
    "//sys/kern:kernel_subsystems",
    "//sys/dev:device_stack",
    "//sys/platform:platform_stack",
    "//sys/net:network_stack",
  },
}
EOF

cat > "$root/sys_arch.qst.tmp" <<'EOF'
qstar.subdir("arm64")
qstar.subdir("amd64")

qstar.group "arch_variants" {
  deps = {
    "//sys/arch/arm64:arch_arm64",
    "//sys/arch/amd64:arch_amd64",
  },
}
EOF
mkdir -p "$root/sys/arch"
mv "$root/sys_arch.qst.tmp" "$root/sys/arch/arch.qst"

cat > "$root/sys_board.qst.tmp" <<'EOF'
qstar.subdir("devkit/a64")
qstar.subdir("sim/virt-aarch64")

qstar.group "board_variants" {
  deps = {
    "//sys/board/devkit/a64:board_devkit_a",
    "//sys/board/sim/virt-aarch64:board_sim_virt_aarch64",
  },
}
EOF
mkdir -p "$root/sys/board"
mv "$root/sys_board.qst.tmp" "$root/sys/board/board.qst"

cat > "$root/sys_kern.qst.tmp" <<'EOF'
qstar.subdir("boot")
qstar.subdir("mm")
qstar.subdir("irq")
qstar.subdir("time")
qstar.subdir("sched")
qstar.subdir("executor")
qstar.subdir("device")
qstar.subdir("vm")
qstar.subdir("syscall")
qstar.subdir("object")
qstar.subdir("sync")

qstar.group "kernel_subsystems" {
  deps = {
    "//sys/kern/boot:kernel_boot",
    "//sys/kern/mm:kernel_mm",
    "//sys/kern/irq:kernel_irq",
    "//sys/kern/time:kernel_time",
    "//sys/kern/sched:kernel_sched",
    "//sys/kern/executor:kernel_executor",
    "//sys/kern/device:kernel_device",
    "//sys/kern/vm:kernel_vm",
    "//sys/kern/syscall:kernel_syscall",
    "//sys/kern/object:kernel_object",
    "//sys/kern/sync:kernel_sync",
  },
}
EOF
mkdir -p "$root/sys/kern"
mv "$root/sys_kern.qst.tmp" "$root/sys/kern/kern.qst"

cat > "$root/sys_dev.qst.tmp" <<'EOF'
qstar.subdir("serial")
qstar.subdir("timer")
qstar.subdir("gpio")
qstar.subdir("pwm")
qstar.subdir("i2c")
qstar.subdir("spi")
qstar.subdir("storage")
qstar.subdir("watchdog")
qstar.subdir("rtc")
qstar.subdir("dma")
qstar.subdir("mailbox")
qstar.subdir("random")

qstar.group "device_stack" {
  deps = {
    "//sys/dev/serial:driver_serial",
    "//sys/dev/timer:driver_timer",
    "//sys/dev/gpio:driver_gpio",
    "//sys/dev/pwm:driver_pwm",
    "//sys/dev/i2c:driver_i2c",
    "//sys/dev/spi:driver_spi",
    "//sys/dev/storage:driver_storage",
    "//sys/dev/watchdog:driver_watchdog",
    "//sys/dev/rtc:driver_rtc",
    "//sys/dev/dma:driver_dma",
    "//sys/dev/mailbox:driver_mailbox",
    "//sys/dev/random:driver_random",
  },
}
EOF
mkdir -p "$root/sys/dev"
mv "$root/sys_dev.qst.tmp" "$root/sys/dev/dev.qst"

cat > "$root/sys_platform.qst.tmp" <<'EOF'
qstar.subdir("clock")
qstar.subdir("power")
qstar.subdir("memory")
qstar.subdir("interrupt")
qstar.subdir("bootflow")
qstar.subdir("firmware")

qstar.group "platform_stack" {
  deps = {
    "//sys/platform/clock:platform_clock",
    "//sys/platform/power:platform_power",
    "//sys/platform/memory:platform_memory",
    "//sys/platform/interrupt:platform_interrupt",
    "//sys/platform/bootflow:platform_bootflow",
    "//sys/platform/firmware:platform_firmware",
  },
}
EOF
mkdir -p "$root/sys/platform"
mv "$root/sys_platform.qst.tmp" "$root/sys/platform/platform.qst"

cat > "$root/sys_net.qst.tmp" <<'EOF'
qstar.subdir("eth")
qstar.subdir("ipv4")
qstar.subdir("udp")
qstar.subdir("bootp")
qstar.subdir("console")
qstar.subdir("telemetry")

qstar.group "network_stack" {
  deps = {
    "//sys/net/eth:net_eth",
    "//sys/net/ipv4:net_ipv4",
    "//sys/net/udp:net_udp",
    "//sys/net/bootp:net_bootp",
    "//sys/net/console:net_console",
    "//sys/net/telemetry:net_telemetry",
  },
}
EOF
mkdir -p "$root/sys/net"
mv "$root/sys_net.qst.tmp" "$root/sys/net/net.qst"

write_staticlib "lib/libk" "libk_core" "libk_core" 1 ""
write_staticlib "sys/arch/arm64" "arch_arm64" "arch_arm64" 2 '"//lib/libk:libk_core"'
write_staticlib "sys/arch/amd64" "arch_amd64" "arch_amd64" 3 '"//lib/libk:libk_core"'
write_staticlib "sys/board/devkit/a64" "board_devkit_a" "board_devkit_a" 4 '"//lib/libk:libk_core"'
write_staticlib "sys/board/sim/virt-aarch64" "board_sim_virt_aarch64" "board_sim_virt_aarch64" 5 '"//lib/libk:libk_core"'
write_staticlib "sys/kern/boot" "kernel_boot" "kernel_boot" 6 '"//lib/libk:libk_core"'
write_staticlib "sys/kern/mm" "kernel_mm" "kernel_mm" 7 '"//lib/libk:libk_core"'
write_staticlib "sys/kern/irq" "kernel_irq" "kernel_irq" 8 '"//lib/libk:libk_core"'
write_staticlib "sys/kern/time" "kernel_time" "kernel_time" 9 '"//lib/libk:libk_core"'
write_staticlib "sys/kern/sched" "kernel_sched" "kernel_sched" 10 '"//lib/libk:libk_core"'
write_staticlib "sys/kern/executor" "kernel_executor" "kernel_executor" 11 '"//lib/libk:libk_core"'
write_staticlib "sys/kern/device" "kernel_device" "kernel_device" 12 '"//lib/libk:libk_core"'
write_staticlib "sys/kern/vm" "kernel_vm" "kernel_vm" 13 '"//lib/libk:libk_core"'
write_staticlib "sys/kern/syscall" "kernel_syscall" "kernel_syscall" 14 '"//lib/libk:libk_core"'
write_staticlib "sys/kern/object" "kernel_object" "kernel_object" 15 '"//lib/libk:libk_core"'
write_staticlib "sys/kern/sync" "kernel_sync" "kernel_sync" 16 '"//lib/libk:libk_core"'
write_staticlib "sys/dev/serial" "driver_serial" "driver_serial" 17 '"//lib/libk:libk_core"'
write_staticlib "sys/dev/timer" "driver_timer" "driver_timer" 18 '"//lib/libk:libk_core"'
write_staticlib "sys/dev/gpio" "driver_gpio" "driver_gpio" 19 '"//lib/libk:libk_core"'
write_staticlib "sys/dev/pwm" "driver_pwm" "driver_pwm" 20 '"//lib/libk:libk_core"'
write_staticlib "sys/dev/i2c" "driver_i2c" "driver_i2c" 21 '"//lib/libk:libk_core"'
write_staticlib "sys/dev/spi" "driver_spi" "driver_spi" 22 '"//lib/libk:libk_core"'
write_staticlib "sys/dev/storage" "driver_storage" "driver_storage" 23 '"//lib/libk:libk_core"'
write_staticlib "sys/dev/watchdog" "driver_watchdog" "driver_watchdog" 24 '"//lib/libk:libk_core"'
write_staticlib "sys/dev/rtc" "driver_rtc" "driver_rtc" 25 '"//lib/libk:libk_core"'
write_staticlib "sys/dev/dma" "driver_dma" "driver_dma" 26 '"//lib/libk:libk_core"'
write_staticlib "sys/dev/mailbox" "driver_mailbox" "driver_mailbox" 27 '"//lib/libk:libk_core"'
write_staticlib "sys/dev/random" "driver_random" "driver_random" 28 '"//lib/libk:libk_core"'
write_staticlib "sys/platform/clock" "platform_clock" "platform_clock" 29 '"//lib/libk:libk_core"'
write_staticlib "sys/platform/power" "platform_power" "platform_power" 30 '"//lib/libk:libk_core"'
write_staticlib "sys/platform/memory" "platform_memory" "platform_memory" 31 '"//lib/libk:libk_core"'
write_staticlib "sys/platform/interrupt" "platform_interrupt" "platform_interrupt" 32 '"//lib/libk:libk_core"'
write_staticlib "sys/platform/bootflow" "platform_bootflow" "platform_bootflow" 33 '"//lib/libk:libk_core"'
write_staticlib "sys/platform/firmware" "platform_firmware" "platform_firmware" 34 '"//lib/libk:libk_core"'
write_staticlib "sys/net/eth" "net_eth" "net_eth" 35 '"//lib/libk:libk_core"'
write_staticlib "sys/net/ipv4" "net_ipv4" "net_ipv4" 36 '"//lib/libk:libk_core"'
write_staticlib "sys/net/udp" "net_udp" "net_udp" 37 '"//lib/libk:libk_core"'
write_staticlib "sys/net/bootp" "net_bootp" "net_bootp" 38 '"//lib/libk:libk_core"'
write_staticlib "sys/net/console" "net_console" "net_console" 39 '"//lib/libk:libk_core"'
write_staticlib "sys/net/telemetry" "net_telemetry" "net_telemetry" 40 '"//lib/libk:libk_core"'

"$qstar" --file "$root/qstar.lua" check //:firmware_image > "$tmp/check.out" 2> "$tmp/check.err"
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

run_timed stella_trace "$qstar" --file "$root/qstar.lua" -B build/stella-trace -G stella build //:firmware_image --schedule-trace --progress off --color never
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

run_timed stella_clean "$qstar" --file "$root/qstar.lua" -B build/stella -G stella build //:firmware_image
contains "$tmp/stella_clean.out" "group_target label=//:firmware_image"
contains "$tmp/stella_clean.out" "status ok"
test -f "$root/build/stella/compile_commands.json" || fail "stella compile_commands missing"
check_elapsed_max "stella clean build" "$stella_clean_ms" "$clean_max_ms"
printf 'medium_project_gate backend=stella phase=clean elapsed_ms=%s\n' "$stella_clean_ms"

run_timed stella_noop "$qstar" --file "$root/qstar.lua" -B build/stella -G stella build //:firmware_image
contains "$tmp/stella_noop.out" "status ok"
check_elapsed_max "stella no-op build" "$stella_noop_ms" "$noop_max_ms"
printf 'medium_project_gate backend=stella phase=noop elapsed_ms=%s\n' "$stella_noop_ms"

bump_source "sys/kern/mm/mm.c" 7001
run_timed stella_incremental "$qstar" --file "$root/qstar.lua" -B build/stella -G stella build //:firmware_image
contains "$tmp/stella_incremental.out" "status ok"
check_elapsed_max "stella incremental build" "$stella_incremental_ms" "$incremental_max_ms"
printf 'medium_project_gate backend=stella phase=incremental elapsed_ms=%s\n' "$stella_incremental_ms"

run_timed stella_jobs_clean "$qstar" --file "$root/qstar.lua" -B build/stella-jobs -G stella build //:firmware_image --jobs "$host_jobs"
contains "$tmp/stella_jobs_clean.out" "group_target label=//:firmware_image"
contains "$tmp/stella_jobs_clean.out" "status ok"
test -f "$root/build/stella-jobs/compile_commands.json" || fail "stella --jobs compile_commands missing"
check_elapsed_max "stella --jobs clean build" "$stella_jobs_clean_ms" "$clean_max_ms"
printf 'medium_project_gate backend=stella-jobs jobs=%s phase=clean elapsed_ms=%s\n' "$host_jobs" "$stella_jobs_clean_ms"

run_timed stella_jobs_noop "$qstar" --file "$root/qstar.lua" -B build/stella-jobs -G stella build //:firmware_image --jobs "$host_jobs"
contains "$tmp/stella_jobs_noop.out" "status ok"
check_elapsed_max "stella --jobs no-op build" "$stella_jobs_noop_ms" "$noop_max_ms"
printf 'medium_project_gate backend=stella-jobs jobs=%s phase=noop elapsed_ms=%s\n' "$host_jobs" "$stella_jobs_noop_ms"

bump_source "sys/kern/mm/mm.c" 7002
run_timed stella_jobs_incremental "$qstar" --file "$root/qstar.lua" -B build/stella-jobs -G stella build //:firmware_image --jobs "$host_jobs"
contains "$tmp/stella_jobs_incremental.out" "status ok"
check_elapsed_max "stella --jobs incremental build" "$stella_jobs_incremental_ms" "$incremental_max_ms"
printf 'medium_project_gate backend=stella-jobs jobs=%s phase=incremental elapsed_ms=%s\n' "$host_jobs" "$stella_jobs_incremental_ms"

"$qstar" --file "$root/qstar.lua" -B build/stella action-log //sys/kern/mm:kernel_mm:archive:0 > "$tmp/kernel-mm-archive-log.out" 2> "$tmp/kernel-mm-archive-log.err"
contains "$tmp/kernel-mm-archive-log.out" "argv[0]=ar"
contains "$tmp/kernel-mm-archive-log.out" "libkernel_mm.a"
not_contains "$tmp/kernel-mm-archive-log.out" "liblibk_core.a"
printf 'medium_project_gate staticlib_argv_parity=ok target=//sys/kern/mm:kernel_mm\n'

if command -v ninja >/dev/null 2>&1; then
	run_timed ninja_clean "$qstar" --file "$root/qstar.lua" -B build/qstar-ninja -G ninja build //:firmware_image
	contains "$tmp/ninja_clean.out" "backend ninja"
	contains "$tmp/ninja_clean.out" "status ok"
	test -f "$root/build/qstar-ninja/compile_commands.json" || fail "ninja compile_commands missing"
	check_elapsed_max "ninja clean build" "$ninja_clean_ms" "$clean_max_ms"
	printf 'medium_project_gate backend=ninja phase=clean elapsed_ms=%s\n' "$ninja_clean_ms"
	run_timed ninja_noop "$qstar" --file "$root/qstar.lua" -B build/qstar-ninja -G ninja build //:firmware_image
	contains "$tmp/ninja_noop.out" "backend ninja"
	contains "$tmp/ninja_noop.out" "status ok"
	check_elapsed_max "ninja no-op build" "$ninja_noop_ms" "$noop_max_ms"
	printf 'medium_project_gate backend=ninja phase=noop elapsed_ms=%s\n' "$ninja_noop_ms"
	bump_source "sys/kern/mm/mm.c" 7003
	run_timed ninja_incremental "$qstar" --file "$root/qstar.lua" -B build/qstar-ninja -G ninja build //:firmware_image
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
	check_stella_vs_ninja clean "$stella_clean_ms" "$ninja_clean_ms"
	check_stella_vs_ninja noop "$stella_noop_ms" "$ninja_noop_ms"
	check_stella_vs_ninja incremental "$stella_incremental_ms" "$ninja_incremental_ms"
	check_stella_vs_ninja clean "$stella_jobs_clean_ms" "$ninja_clean_ms"
	check_stella_vs_ninja noop "$stella_jobs_noop_ms" "$ninja_noop_ms"
	check_stella_vs_ninja incremental "$stella_jobs_incremental_ms" "$ninja_incremental_ms"
else
	printf 'medium_project_gate backend=ninja phase=clean elapsed_ms=skipped reason=ninja-not-found\n'
	printf 'medium_project_gate backend=ninja phase=noop elapsed_ms=skipped reason=ninja-not-found\n'
	printf 'medium_project_gate backend=ninja phase=incremental elapsed_ms=skipped reason=ninja-not-found\n'
fi

printf 'medium_project_gate status=ok perf_issue_count=%s report_only=%s\n' "$perf_issue_count" "$report_only"
