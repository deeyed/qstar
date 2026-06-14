#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-large-project-performance.$$
modes=${QSTAR_LARGE_PROJECT_TARGETS:-"200 500"}
object_count=${QSTAR_LARGE_OBJECT_BRIDGE_COUNT:-4}
ratio_x100=${QSTAR_LARGE_STELLA_TO_NINJA_X100:-200}
ratio_slack_ms=${QSTAR_LARGE_RATIO_SLACK_MS:-500}
report_only=${QSTAR_LARGE_PERF_REPORT_ONLY:-1}
perf_issue_count=0

fail() {
	echo "qstar-large-project: $*" >&2
	exit 1
}

contains() {
	file=$1
	pat=$2
	if ! grep -F -q -- "$pat" "$file"; then
		sed -n '1,120p' "$file" >&2 || true
		fail "missing pattern '$pat' in $file"
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

perf_issue() {
	perf_issue_count=$((perf_issue_count + 1))
	if [ "$report_only" = 1 ]; then
		printf 'large_project_gate warning=%s\n' "$*"
	else
		fail "$*"
	fi
}

check_stella_vs_ninja() {
	mode=$1
	backend=$2
	phase=$3
	stella_elapsed=$4
	ninja_elapsed=$5
	if [ $((stella_elapsed * 100)) -gt $((ninja_elapsed * ratio_x100 + ratio_slack_ms * 100)) ]; then
		perf_issue "mode=${mode} backend=${backend} phase=${phase} stella ${stella_elapsed}ms exceeds ninja ${ninja_elapsed}ms beyond ratio_x100=${ratio_x100} slack_ms=${ratio_slack_ms}"
	fi
}

bump_source() {
	root=$1
	index=$2
	value=$3
	sleep 1
	name=$(printf 'lib%04d' "$index")
	func=$(printf 'large_lib_%04d' "$index")
	printf 'int %s(void) { return %s; }\n' "$func" "$value" > "$root/src/$name.c"
}

write_foreign_compiler() {
	root=$1
	mkdir -p "$root/tools"
	cat > "$root/tools/fake-foreign-compile.sh" <<'EOF'
#!/bin/sh
set -eu

input=$1
output=$2
symbol=$3
tmp="${output}.c"

test -f "$input"
mkdir -p "$(dirname "$output")"
cat > "$tmp" <<EOF_C
int
${symbol}(void)
{
	return 9001;
}
EOF_C

${CC:-cc} -fPIC -c "$tmp" -o "$output"
EOF
	chmod +x "$root/tools/fake-foreign-compile.sh"
}

write_project() {
	root=$1
	mode=$2
	if [ "$mode" -ge 500 ]; then
		app_count=5
	elif [ "$mode" -ge 200 ]; then
		app_count=2
	else
		app_count=1
	fi
	lib_count=$((mode - app_count - 1))
	if [ "$lib_count" -lt 1 ]; then
		fail "mode ${mode} is too small"
	fi
	if [ "$object_count" -gt "$lib_count" ]; then
		current_object_count=$lib_count
	else
		current_object_count=$object_count
	fi
	mkdir -p "$root/src" "$root/foreign"
	write_foreign_compiler "$root"
	cat > "$root/qstar.lua" <<EOF
qstar.project {
  name = "large-synthetic-${mode}",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.config "large_c" {
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

EOF
	i=1
	while [ "$i" -le "$current_object_count" ]; do
		obj=$(printf 'foreign_%04d' "$i")
		sym=$(printf 'large_foreign_%04d' "$i")
		printf 'foreign payload %s\n' "$i" > "$root/foreign/$obj.ext"
		cat >> "$root/qstar.lua" <<EOF
qstar.custom_target "$obj" {
  inputs = {
    "foreign/$obj.ext",
  },
  outputs = {
    qstar.output("build/qstar/generated/foreign/$obj.o", {
      format = "object",
    }),
  },
  command = qstar.cli {
    "tools/fake-foreign-compile.sh",
    qstar.input(0),
    qstar.output(0),
    "$sym",
  },
  description = qstar.status("Building foreign object $obj.o"),
}

EOF
		i=$((i + 1))
	done
	i=1
	while [ "$i" -le "$lib_count" ]; do
		name=$(printf 'lib%04d' "$i")
		func=$(printf 'large_lib_%04d' "$i")
		printf 'int %s(void) { return %s; }\n' "$func" "$i" > "$root/src/$name.c"
		cat >> "$root/qstar.lua" <<EOF
qstar.staticlib "$name" {
  configs = {"//:large_c"},
  sources = {
    "src/$name.c",
EOF
		if [ "$i" -le "$current_object_count" ]; then
			obj=$(printf 'foreign_%04d' "$i")
			printf '    qstar.output("build/qstar/generated/foreign/%s.o"),\n' "$obj" >> "$root/qstar.lua"
		fi
		cat >> "$root/qstar.lua" <<'EOF'
  },
EOF
		if [ "$i" -gt 1 ]; then
			cat >> "$root/qstar.lua" <<'EOF'
  deps = {
    "//:lib0001",
  },
EOF
		fi
		cat >> "$root/qstar.lua" <<'EOF'
}

EOF
		i=$((i + 1))
	done
	app=1
	while [ "$app" -le "$app_count" ]; do
		app_name=$(printf 'large_app_%04d' "$app")
		app_src=$(printf 'app_%04d' "$app")
		start=$(( (app - 1) * lib_count / app_count + 1 ))
		end=$(( app * lib_count / app_count ))
		cat > "$root/src/$app_src.c" <<'EOF'
int
main(void)
{
	return 0;
}
EOF
		cat >> "$root/qstar.lua" <<EOF
qstar.executable "$app_name" {
  configs = {"//:large_c"},
  sources = {
    "src/$app_src.c",
  },
  deps = {
EOF
		i=$start
		while [ "$i" -le "$end" ]; do
			name=$(printf 'lib%04d' "$i")
			printf '    "//:%s",\n' "$name" >> "$root/qstar.lua"
			i=$((i + 1))
		done
		cat >> "$root/qstar.lua" <<'EOF'
  },
}

EOF
		app=$((app + 1))
	done
	cat >> "$root/qstar.lua" <<'EOF'
qstar.group "all" {
  deps = {
EOF
	app=1
	while [ "$app" -le "$app_count" ]; do
		app_name=$(printf 'large_app_%04d' "$app")
		printf '    "//:%s",\n' "$app_name" >> "$root/qstar.lua"
		app=$((app + 1))
	done
	cat >> "$root/qstar.lua" <<'EOF'
  },
}
EOF
}

run_mode() {
	mode=$1
	check_root="$tmp/project-$mode-check"
	rm -rf "$check_root"
	mkdir -p "$check_root"
	write_project "$check_root" "$mode"
	host_jobs=$(detect_host_jobs)
	"$qstar" --file "$check_root/qstar.lua" check //:all > "$tmp/check-$mode.out" 2> "$tmp/check-$mode.err"
	contains "$tmp/check-$mode.out" "status ok"
	target_count=$(awk '/^target-count / {print $2}' "$tmp/check-$mode.out")
	generated_count=$(awk '/^generated-action-count / {print $2}' "$tmp/check-$mode.out")
	if [ -z "$target_count" ] || [ "$target_count" -lt "$mode" ]; then
		fail "mode=${mode} target_count=${target_count:-missing}"
	fi
	if [ -z "$generated_count" ] || [ "$generated_count" -lt 1 ]; then
		fail "mode=${mode} generated_action_count=${generated_count:-missing}"
	fi
	"$qstar" --file "$check_root/qstar.lua" lint //... > "$tmp/lint-$mode.out" 2> "$tmp/lint-$mode.err"
	contains "$tmp/lint-$mode.out" "status ok"
	printf 'large_project_gate mode=%s target_count=%s generated_actions=%s host_jobs=%s\n' "$mode" "$target_count" "$generated_count" "$host_jobs"

	stella_root="$tmp/project-$mode-stella"
	rm -rf "$stella_root"
	mkdir -p "$stella_root"
	write_project "$stella_root" "$mode"
	run_timed "stella_clean_$mode" "$qstar" --file "$stella_root/qstar.lua" -B build/stella -G stella build //:all --progress off --color never
	eval "stella_clean_ms=\$stella_clean_${mode}_ms"
	contains "$tmp/stella_clean_$mode.out" "status ok"
	test -f "$stella_root/build/stella/compile_commands.json" || fail "mode=${mode} stella compile_commands missing"
	i=1
	while [ "$i" -le "$object_count" ] && [ "$i" -le "$((mode - 2))" ]; do
		obj=$(printf 'foreign_%04d' "$i")
		test -f "$stella_root/build/qstar/generated/foreign/$obj.o" || fail "mode=${mode} object bridge missing $obj.o"
		i=$((i + 1))
	done
	printf 'large_project_gate mode=%s backend=stella phase=clean elapsed_ms=%s\n' "$mode" "$stella_clean_ms"

	run_timed "stella_noop_$mode" "$qstar" --file "$stella_root/qstar.lua" -B build/stella -G stella build //:all --progress off --color never
	eval "stella_noop_ms=\$stella_noop_${mode}_ms"
	contains "$tmp/stella_noop_$mode.out" "status ok"
	printf 'large_project_gate mode=%s backend=stella phase=noop elapsed_ms=%s\n' "$mode" "$stella_noop_ms"

	bump_source "$stella_root" 2 7001
	run_timed "stella_incremental_$mode" "$qstar" --file "$stella_root/qstar.lua" -B build/stella -G stella build //:all --progress off --color never
	eval "stella_incremental_ms=\$stella_incremental_${mode}_ms"
	contains "$tmp/stella_incremental_$mode.out" "status ok"
	printf 'large_project_gate mode=%s backend=stella phase=incremental elapsed_ms=%s\n' "$mode" "$stella_incremental_ms"

	stella_jobs_root="$tmp/project-$mode-stella-jobs"
	rm -rf "$stella_jobs_root"
	mkdir -p "$stella_jobs_root"
	write_project "$stella_jobs_root" "$mode"
	run_timed "stella_jobs_clean_$mode" "$qstar" --file "$stella_jobs_root/qstar.lua" -B build/stella-jobs -G stella build //:all --jobs "$host_jobs" --progress off --color never
	eval "stella_jobs_clean_ms=\$stella_jobs_clean_${mode}_ms"
	contains "$tmp/stella_jobs_clean_$mode.out" "status ok"
	test -f "$stella_jobs_root/build/stella-jobs/compile_commands.json" || fail "mode=${mode} stella --jobs compile_commands missing"
	printf 'large_project_gate mode=%s backend=stella-jobs jobs=%s phase=clean elapsed_ms=%s\n' "$mode" "$host_jobs" "$stella_jobs_clean_ms"

	run_timed "stella_jobs_noop_$mode" "$qstar" --file "$stella_jobs_root/qstar.lua" -B build/stella-jobs -G stella build //:all --jobs "$host_jobs" --progress off --color never
	eval "stella_jobs_noop_ms=\$stella_jobs_noop_${mode}_ms"
	contains "$tmp/stella_jobs_noop_$mode.out" "status ok"
	printf 'large_project_gate mode=%s backend=stella-jobs jobs=%s phase=noop elapsed_ms=%s\n' "$mode" "$host_jobs" "$stella_jobs_noop_ms"

	bump_source "$stella_jobs_root" 3 7002
	run_timed "stella_jobs_incremental_$mode" "$qstar" --file "$stella_jobs_root/qstar.lua" -B build/stella-jobs -G stella build //:all --jobs "$host_jobs" --progress off --color never
	eval "stella_jobs_incremental_ms=\$stella_jobs_incremental_${mode}_ms"
	contains "$tmp/stella_jobs_incremental_$mode.out" "status ok"
	printf 'large_project_gate mode=%s backend=stella-jobs jobs=%s phase=incremental elapsed_ms=%s\n' "$mode" "$host_jobs" "$stella_jobs_incremental_ms"

	if command -v ninja >/dev/null 2>&1; then
		ninja_root="$tmp/project-$mode-ninja"
		rm -rf "$ninja_root"
		mkdir -p "$ninja_root"
		write_project "$ninja_root" "$mode"
		run_timed "ninja_clean_$mode" "$qstar" --file "$ninja_root/qstar.lua" -B build/ninja -G ninja build //:all --progress off --color never
		eval "ninja_clean_ms=\$ninja_clean_${mode}_ms"
		contains "$tmp/ninja_clean_$mode.out" "backend ninja"
		contains "$tmp/ninja_clean_$mode.out" "status ok"
		test -f "$ninja_root/build/ninja/compile_commands.json" || fail "mode=${mode} ninja compile_commands missing"
		test ! -f "$ninja_root/.ninja_log" || fail "mode=${mode} ninja wrote root .ninja_log"
		test ! -f "$ninja_root/.ninja_deps" || fail "mode=${mode} ninja wrote root .ninja_deps"
		printf 'large_project_gate mode=%s backend=ninja phase=clean elapsed_ms=%s\n' "$mode" "$ninja_clean_ms"
		run_timed "ninja_noop_$mode" "$qstar" --file "$ninja_root/qstar.lua" -B build/ninja -G ninja build //:all --progress off --color never
		eval "ninja_noop_ms=\$ninja_noop_${mode}_ms"
		contains "$tmp/ninja_noop_$mode.out" "backend ninja"
		contains "$tmp/ninja_noop_$mode.out" "status ok"
		printf 'large_project_gate mode=%s backend=ninja phase=noop elapsed_ms=%s\n' "$mode" "$ninja_noop_ms"
		bump_source "$ninja_root" 4 7003
		run_timed "ninja_incremental_$mode" "$qstar" --file "$ninja_root/qstar.lua" -B build/ninja -G ninja build //:all --progress off --color never
		eval "ninja_incremental_ms=\$ninja_incremental_${mode}_ms"
		contains "$tmp/ninja_incremental_$mode.out" "backend ninja"
		contains "$tmp/ninja_incremental_$mode.out" "status ok"
		printf 'large_project_gate mode=%s backend=ninja phase=incremental elapsed_ms=%s\n' "$mode" "$ninja_incremental_ms"
		printf 'large_project_gate mode=%s compare phase=clean stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$mode" "$stella_clean_ms" "$ninja_clean_ms" "$ratio_x100" "$ratio_slack_ms"
		printf 'large_project_gate mode=%s compare phase=noop stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$mode" "$stella_noop_ms" "$ninja_noop_ms" "$ratio_x100" "$ratio_slack_ms"
		printf 'large_project_gate mode=%s compare phase=incremental stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$mode" "$stella_incremental_ms" "$ninja_incremental_ms" "$ratio_x100" "$ratio_slack_ms"
		printf 'large_project_gate mode=%s compare backend=stella-jobs phase=clean stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$mode" "$stella_jobs_clean_ms" "$ninja_clean_ms" "$ratio_x100" "$ratio_slack_ms"
		printf 'large_project_gate mode=%s compare backend=stella-jobs phase=noop stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$mode" "$stella_jobs_noop_ms" "$ninja_noop_ms" "$ratio_x100" "$ratio_slack_ms"
		printf 'large_project_gate mode=%s compare backend=stella-jobs phase=incremental stella_ms=%s ninja_ms=%s ratio_x100=%s slack_ms=%s\n' "$mode" "$stella_jobs_incremental_ms" "$ninja_incremental_ms" "$ratio_x100" "$ratio_slack_ms"
		check_stella_vs_ninja "$mode" stella clean "$stella_clean_ms" "$ninja_clean_ms"
		check_stella_vs_ninja "$mode" stella noop "$stella_noop_ms" "$ninja_noop_ms"
		check_stella_vs_ninja "$mode" stella incremental "$stella_incremental_ms" "$ninja_incremental_ms"
		check_stella_vs_ninja "$mode" stella-jobs clean "$stella_jobs_clean_ms" "$ninja_clean_ms"
		check_stella_vs_ninja "$mode" stella-jobs noop "$stella_jobs_noop_ms" "$ninja_noop_ms"
		check_stella_vs_ninja "$mode" stella-jobs incremental "$stella_jobs_incremental_ms" "$ninja_incremental_ms"
	else
		printf 'large_project_gate mode=%s backend=ninja phase=clean elapsed_ms=skipped reason=ninja-not-found\n' "$mode"
		printf 'large_project_gate mode=%s backend=ninja phase=noop elapsed_ms=skipped reason=ninja-not-found\n' "$mode"
		printf 'large_project_gate mode=%s backend=ninja phase=incremental elapsed_ms=skipped reason=ninja-not-found\n' "$mode"
	fi
}

rm -rf "$tmp"
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

for mode in $modes; do
	case "$mode" in
	''|*[!0-9]*)
		fail "invalid large project mode '$mode'"
		;;
	esac
	run_mode "$mode"
done

printf 'large_project_gate status=ok perf_issue_count=%s report_only=%s modes="%s"\n' "$perf_issue_count" "$report_only" "$modes"
