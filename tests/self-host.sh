#!/bin/sh
set -eu

fail() {
	printf 'qstar-self-host: %s\n' "$1" >&2
	exit 1
}

contains() {
	file=$1
	pattern=$2
	if ! grep -F -- "$pattern" "$file" >/dev/null 2>&1; then
		fail "missing pattern '$pattern' in $file"
	fi
}

run_step() {
	name=$1
	out=$2
	err=$3
	shift 3
	if ! "$@" > "$out" 2> "$err"; then
		printf 'qstar-self-host: command failed: %s\n' "$name" >&2
		if [ -f "$out" ]; then
			printf '%s\n' "--- $name.out ---" >&2
			tail -n 80 "$out" >&2 || true
		fi
		if [ -f "$err" ]; then
			printf '%s\n' "--- $name.err ---" >&2
			tail -n 80 "$err" >&2 || true
		fi
		exit 1
	fi
}

same_file() {
	left=$1
	right=$2
	if ! cmp -s "$left" "$right"; then
		printf 'qstar-self-host: %s:\n' "$left" >&2
		cat "$left" >&2
		printf 'qstar-self-host: %s:\n' "$right" >&2
		cat "$right" >&2
		fail "files differ: $left $right"
	fi
}

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
stella_build=${QSTAR_SELF_HOST_BUILD_DIR:-build/qstar-self}
ninja_build=${QSTAR_SELF_HOST_NINJA_BUILD_DIR:-build/qstar-self-ninja}
stella_bin="$stella_build/out/___qstar/qstar"
ninja_bin="$ninja_build/out/___qstar/qstar"

rm -rf "$stella_build" "$ninja_build"

run_step makefile-version "$stella_build.makefile-version.out" "$stella_build.makefile-version.err" \
	"$qstar" --version
contains "$stella_build.makefile-version.out" "qstar "

run_step check "$stella_build.check.out" "$stella_build.check.err" \
	"$qstar" --file qstar.lua check
contains "$stella_build.check.out" "status ok"

run_step stella-build "$stella_build.build.out" "$stella_build.build.err" \
	"$qstar" --file qstar.lua -B "$stella_build" build //:qstar --progress off
contains "$stella_build.build.out" "status ok"
test -x "$stella_bin" || fail "Stella self-host binary missing"
run_step stella-version "$stella_build.version.out" "$stella_build.version.err" \
	"$stella_bin" --version
contains "$stella_build.version.out" "qstar "
same_file "$stella_build.makefile-version.out" "$stella_build.version.out"
run_step stella-sample-check "$stella_build.sample.out" "$stella_build.sample.err" \
	"$stella_bin" --file tests/projects/c-app-lib-test/qstar.lua check
contains "$stella_build.sample.out" "status ok"
run_step stella-self-check "$stella_build.self-check.out" "$stella_build.self-check.err" \
	"$stella_bin" --file qstar.lua check
contains "$stella_build.self-check.out" "status ok"
test -f "$stella_build/compile_commands.json" || fail "Stella compile database missing"

run_step stella-self-host "$stella_build.self-host.out" "$stella_build.self-host.err" \
	"$qstar" --file qstar.lua -B "$stella_build" build //:self_host --progress off
contains "$stella_build.self-host.out" "run_expect label=//:self_version status=matched"
contains "$stella_build.self-host.out" "run_expect label=//:self_check_sample status=matched"
contains "$stella_build.self-host.out" "run_expect label=//:self_check_graph status=matched"

run_step ninja-build "$ninja_build.build.out" "$ninja_build.build.err" \
	"$qstar" --file qstar.lua -B "$ninja_build" -G ninja build //:qstar --progress off
contains "$ninja_build.build.out" "backend ninja"
contains "$ninja_build.build.out" "status ok"
test -x "$ninja_bin" || fail "Ninja self-host binary missing"
run_step ninja-version "$ninja_build.version.out" "$ninja_build.version.err" \
	"$ninja_bin" --version
contains "$ninja_build.version.out" "qstar "
same_file "$stella_build.makefile-version.out" "$ninja_build.version.out"
test -f "$ninja_build/compile_commands.json" || fail "Ninja compile database missing"
test ! -f .ninja_log || fail "Ninja backend wrote root .ninja_log"
test ! -f .ninja_deps || fail "Ninja backend wrote root .ninja_deps"

printf 'qstar-self-host: passed\n'
