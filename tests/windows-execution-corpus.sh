#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
profile=${QSTAR_WINDOWS_EXECUTION_PROFILE:-msys2-ucrt64-gcc}
tmp=${TMPDIR:-/tmp}/qstar-windows-execution.$$
corpus=tests/corpus/windows-execution
build_dir=build/qstar

fail() {
	printf 'qstar-windows-execution: %s\n' "$1" >&2
	exit 1
}

contains() {
	file=$1
	pattern=$2
	grep -F -q -- "$pattern" "$file" ||
		fail "missing pattern '$pattern' in $file"
}

run_artifact() {
	artifact=$1
	out=$2
	test -x "$artifact" || fail "missing executable artifact $artifact"
	"$artifact" > "$out" 2>&1
}

rm -rf "$tmp"
mkdir -p "$tmp"
rm -rf "$corpus/build" "$corpus/stage"
rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps"
trap 'rm -rf "$tmp"; rm -rf "$corpus/build" "$corpus/stage"; rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps"' EXIT HUP INT TERM

host=$(uname -s 2>/dev/null || printf unknown)
case "$host" in
MINGW*|MSYS*|CYGWIN*)
	mode=native-windows-execution
	;;
*)
	mode=contract-execution
	;;
esac

printf 'qstar-windows-execution: host=%s mode=%s profile-removed=%s\n' "$host" "$mode" "$profile"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-target x86_64-w64-mingw32 check \
	> "$tmp/check.out" 2> "$tmp/check.err"
contains "$tmp/check.out" "status ok"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-target x86_64-w64-mingw32 dry-run //:bridge_app \
	> "$tmp/bridge-dry.out" 2> "$tmp/bridge-dry.err"
contains "$tmp/bridge-dry.out" "Building external object bridge_payload.o"
contains "$tmp/bridge-dry.out" "build/qstar/generated/bridge/bridge_payload.o"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-target x86_64-w64-mingw32 --progress plain \
	build //:hello > "$tmp/hello-build.out" 2> "$tmp/hello-build.err"
contains "$tmp/hello-build.out" "status ok"
run_artifact "$corpus/$build_dir/out/___hello/hello.exe" "$tmp/hello-run.out"
contains "$tmp/hello-run.out" "windows-execution hello"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-target x86_64-w64-mingw32 --progress plain \
	build //:app > "$tmp/app-build.out" 2> "$tmp/app-build.err"
contains "$tmp/app-build.out" "status ok"
test -f "$corpus/$build_dir/out/___core/libwinexec_core.a" ||
	fail "static library artifact missing"
contains "$corpus/$build_dir/compile_commands.json" "src/app.c"
run_artifact "$corpus/$build_dir/out/___app/app.exe" "$tmp/app-run.out"
contains "$tmp/app-run.out" "windows-execution app core=42"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-target x86_64-w64-mingw32 --progress plain \
	build //:response_probe > "$tmp/response-build.out" 2> "$tmp/response-build.err"
contains "$tmp/response-build.out" "response_file id=//:response_probe:compile:0"
contains "$tmp/response-build.out" "status ok"
test -f "$corpus/$build_dir/rsp/___response_probe_compile_0.rsp" ||
	fail "response probe compile response file missing"
run_artifact "$corpus/$build_dir/out/___response_probe/response_probe.exe" \
	"$tmp/response-run.out"
contains "$tmp/response-run.out" "windows-execution response-probe"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-target x86_64-w64-mingw32 --progress plain \
	build //:bridge_app > "$tmp/bridge-build.out" 2> "$tmp/bridge-build.err"
contains "$tmp/bridge-build.out" "Building external object bridge_payload.o"
contains "$tmp/bridge-build.out" "status ok"
test -f "$corpus/$build_dir/generated/bridge/bridge_payload.o" ||
	fail "generated object bridge output missing"
run_artifact "$corpus/$build_dir/out/___bridge_app/bridge_app.exe" \
	"$tmp/bridge-run.out"
contains "$tmp/bridge-run.out" "windows-execution bridge=77"

prefix="$tmp/prefix"
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-target x86_64-w64-mingw32 install //:app \
	--prefix "$prefix" > "$tmp/install-app.out" 2> "$tmp/install-app.err"
test -f "$prefix/bin/app.exe" || fail "installed app.exe missing"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-target x86_64-w64-mingw32 install //:core \
	--prefix "$prefix" > "$tmp/install-core.out" 2> "$tmp/install-core.err"
test -f "$prefix/lib/libwinexec_core.a" ||
	fail "installed static library missing"

printf 'qstar-windows-execution: passed\n'
