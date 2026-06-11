#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-parus-skeleton-benchmark.$$
root=$tmp/project

fail() {
	echo "qstar-parus-benchmark: $*" >&2
	exit 1
}

contains() {
	file=$1
	pat=$2
	grep -F -q -- "$pat" "$file" || fail "missing pattern '$pat' in $file"
}

now_ms() {
	if command -v python3 >/dev/null 2>&1; then
		python3 -c 'import time; print(int(time.time() * 1000))'
	else
		printf '%s000\n' "$(date +%s)"
	fi
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
  name = "parus-skeleton-benchmark",
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

qstar.group "parus_kernel" {
  deps = {
    "//lib/libk:parus_libk",
    "//sys/arch:parus_arch",
    "//sys/board:parus_board",
    "//sys/kern:parus_kern",
    "//sys/dev:parus_dev",
  },
}
EOF

cat > "$root/sys_arch.qst.tmp" <<'EOF'
qstar.subdir("arm64")
qstar.subdir("amd64")

qstar.group "parus_arch" {
  deps = {
    "//sys/arch/arm64:parus_arch_arm64",
    "//sys/arch/amd64:parus_arch_amd64",
  },
}
EOF
mkdir -p "$root/sys/arch"
mv "$root/sys_arch.qst.tmp" "$root/sys/arch/arch.qst"

cat > "$root/sys_board.qst.tmp" <<'EOF'
qstar.subdir("raspberrypi/rpi5")
qstar.subdir("qemu/virt-aarch64")

qstar.group "parus_board" {
  deps = {
    "//sys/board/raspberrypi/rpi5:parus_board_rpi5",
    "//sys/board/qemu/virt-aarch64:parus_board_qemu_virt_aarch64",
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

qstar.group "parus_kern" {
  deps = {
    "//sys/kern/boot:parus_kern_boot",
    "//sys/kern/mm:parus_kern_mm",
    "//sys/kern/irq:parus_kern_irq",
    "//sys/kern/time:parus_kern_time",
    "//sys/kern/sched:parus_kern_sched",
    "//sys/kern/executor:parus_kern_executor",
    "//sys/kern/device:parus_kern_device",
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

qstar.group "parus_dev" {
  deps = {
    "//sys/dev/serial:parus_dev_serial",
    "//sys/dev/timer:parus_dev_timer",
    "//sys/dev/gpio:parus_dev_gpio",
    "//sys/dev/pwm:parus_dev_pwm",
    "//sys/dev/i2c:parus_dev_i2c",
    "//sys/dev/spi:parus_dev_spi",
    "//sys/dev/storage:parus_dev_storage",
  },
}
EOF
mkdir -p "$root/sys/dev"
mv "$root/sys_dev.qst.tmp" "$root/sys/dev/dev.qst"

write_staticlib "lib/libk" "parus_libk" "parus_libk" 1 ""
write_staticlib "sys/arch/arm64" "parus_arch_arm64" "parus_arch_arm64" 2 '"//lib/libk:parus_libk"'
write_staticlib "sys/arch/amd64" "parus_arch_amd64" "parus_arch_amd64" 3 '"//lib/libk:parus_libk"'
write_staticlib "sys/board/raspberrypi/rpi5" "parus_board_rpi5" "parus_board_rpi5" 4 '"//lib/libk:parus_libk"'
write_staticlib "sys/board/qemu/virt-aarch64" "parus_board_qemu_virt_aarch64" "parus_board_qemu_virt_aarch64" 5 '"//lib/libk:parus_libk"'
write_staticlib "sys/kern/boot" "parus_kern_boot" "parus_kern_boot" 6 '"//lib/libk:parus_libk"'
write_staticlib "sys/kern/mm" "parus_kern_mm" "parus_kern_mm" 7 '"//lib/libk:parus_libk"'
write_staticlib "sys/kern/irq" "parus_kern_irq" "parus_kern_irq" 8 '"//lib/libk:parus_libk"'
write_staticlib "sys/kern/time" "parus_kern_time" "parus_kern_time" 9 '"//lib/libk:parus_libk"'
write_staticlib "sys/kern/sched" "parus_kern_sched" "parus_kern_sched" 10 '"//lib/libk:parus_libk"'
write_staticlib "sys/kern/executor" "parus_kern_executor" "parus_kern_executor" 11 '"//lib/libk:parus_libk"'
write_staticlib "sys/kern/device" "parus_kern_device" "parus_kern_device" 12 '"//lib/libk:parus_libk"'
write_staticlib "sys/dev/serial" "parus_dev_serial" "parus_dev_serial" 13 '"//lib/libk:parus_libk"'
write_staticlib "sys/dev/timer" "parus_dev_timer" "parus_dev_timer" 14 '"//lib/libk:parus_libk"'
write_staticlib "sys/dev/gpio" "parus_dev_gpio" "parus_dev_gpio" 15 '"//lib/libk:parus_libk"'
write_staticlib "sys/dev/pwm" "parus_dev_pwm" "parus_dev_pwm" 16 '"//lib/libk:parus_libk"'
write_staticlib "sys/dev/i2c" "parus_dev_i2c" "parus_dev_i2c" 17 '"//lib/libk:parus_libk"'
write_staticlib "sys/dev/spi" "parus_dev_spi" "parus_dev_spi" 18 '"//lib/libk:parus_libk"'
write_staticlib "sys/dev/storage" "parus_dev_storage" "parus_dev_storage" 19 '"//lib/libk:parus_libk"'

run_timed qstar_graph "$qstar" --file "$root/qstar.lua" -B build/qstar-graph -G qstar_graph build //:parus_kernel
contains "$tmp/qstar_graph.out" "group_target label=//:parus_kernel"
contains "$tmp/qstar_graph.out" "status ok"
test -f "$root/build/qstar-graph/compile_commands.json" || fail "qstar_graph compile_commands missing"
if [ "$qstar_graph_ms" -gt 120000 ]; then
	fail "qstar_graph skeleton build took ${qstar_graph_ms}ms"
fi

printf 'parus_skeleton_benchmark backend=qstar_graph elapsed_ms=%s\n' "$qstar_graph_ms"

if command -v ninja >/dev/null 2>&1; then
	run_timed ninja "$qstar" --file "$root/qstar.lua" -B build/qstar-ninja -G ninja build //:parus_kernel
	contains "$tmp/ninja.out" "backend ninja"
	contains "$tmp/ninja.out" "status ok"
	test -f "$root/build/qstar-ninja/compile_commands.json" || fail "ninja compile_commands missing"
	if [ "$ninja_ms" -gt 120000 ]; then
		fail "ninja skeleton build took ${ninja_ms}ms"
	fi
	printf 'parus_skeleton_benchmark backend=ninja elapsed_ms=%s\n' "$ninja_ms"
	printf 'parus_skeleton_benchmark compare qstar_graph_ms=%s ninja_ms=%s\n' "$qstar_graph_ms" "$ninja_ms"
else
	printf 'parus_skeleton_benchmark backend=ninja elapsed_ms=skipped reason=ninja-not-found\n'
fi

printf 'parus_skeleton_benchmark status=ok\n'
