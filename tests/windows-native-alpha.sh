#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-windows-native-alpha.$$
corpus=tests/corpus/response-files
artifact_dir=${QSTAR_WINDOWS_NATIVE_ALPHA_ARTIFACT_DIR:-}

fail() {
	printf 'qstar-windows-native-alpha: %s\n' "$1" >&2
	exit 1
}

contains() {
	file=$1
	pattern=$2
	grep -F -q -- "$pattern" "$file" ||
		fail "missing pattern '$pattern' in $file"
}

copy_if_exists() {
	src=$1
	dst=$2
	if test -e "$src"; then
		rm -rf "$dst"
		mkdir -p "$(dirname "$dst")"
		cp -R "$src" "$dst"
	fi
}

collect_failure_artifacts() {
	rc=$1
	if test "$rc" -eq 0 || test -z "$artifact_dir"; then
		return 0
	fi
	mkdir -p "$artifact_dir/tmp" "$artifact_dir/corpus"
	printf 'status=fail script=windows-native-alpha rc=%s tmp=%s\n' "$rc" "$tmp" \
		> "$artifact_dir/failure.status"
	if test -d "$tmp"; then
		find "$tmp" -maxdepth 1 -type f -exec cp {} "$artifact_dir/tmp/" \;
	fi
	copy_if_exists "$corpus/build" "$artifact_dir/corpus/build"
	copy_if_exists "$corpus/stage" "$artifact_dir/corpus/stage"
	copy_if_exists "$corpus/.ninja_log" "$artifact_dir/corpus/.ninja_log"
	copy_if_exists "$corpus/.ninja_deps" "$artifact_dir/corpus/.ninja_deps"
}

host=$(uname -s 2>/dev/null || printf unknown)
case "$host" in
MINGW*|MSYS*|CYGWIN*)
	mode=native-windows-alpha
	;;
*)
	mode=contract-only
	;;
esac

rm -rf "$tmp"
mkdir -p "$tmp"
trap 'rc=$?; trap - EXIT HUP INT TERM; collect_failure_artifacts "$rc"; rm -rf "$tmp"; exit "$rc"' EXIT HUP INT TERM

printf 'qstar-windows-native-alpha: host=%s mode=%s\n' "$host" "$mode"
printf 'qstar-windows-native-alpha: baseline=msys2-ucrt64-gcc\n'
printf 'qstar-windows-native-alpha: daemon_named_pipe=deferred\n'

"$qstar" --version > "$tmp/version.out" 2> "$tmp/version.err"
contains "$tmp/version.out" "qstar "

stub_cc=${CC:-cc}
if command -v "$stub_cc" >/dev/null 2>&1; then
	stub_cflags="-D_WIN32 -std=c99 -Wall -Wextra -Wpedantic -Wno-unused-function"
	"$stub_cc" $stub_cflags \
		-Iinclude -Ivendor/lua -c src/daemon.c -o "$tmp/daemon-win-stub.o"
	test -s "$tmp/daemon-win-stub.o" ||
		fail "Windows daemon stub object was not created"
	printf 'qstar-windows-native-alpha: daemon_stub=compiled cc=%s\n' "$stub_cc"
	"$stub_cc" $stub_cflags \
		-Iinclude -Ivendor/lua -c src/executor.c -o "$tmp/executor-win-stub.o"
	test -s "$tmp/executor-win-stub.o" ||
		fail "Windows executor stub object was not created"
	printf 'qstar-windows-native-alpha: executor_stub=compiled cc=%s\n' "$stub_cc"
	"$stub_cc" $stub_cflags \
		-Iinclude -Ivendor/lua -c src/platform_process.c -o "$tmp/platform-process-win-stub.o"
	test -s "$tmp/platform-process-win-stub.o" ||
		fail "Windows platform process stub object was not created"
	printf 'qstar-windows-native-alpha: platform_process_stub=compiled cc=%s\n' "$stub_cc"
	"$stub_cc" $stub_cflags \
		-Iinclude -Ivendor/lua -c src/ninja.c -o "$tmp/ninja-win-stub.o"
	test -s "$tmp/ninja-win-stub.o" ||
		fail "Windows ninja stub object was not created"
	printf 'qstar-windows-native-alpha: ninja_stub=compiled cc=%s\n' "$stub_cc"
else
	printf 'qstar-windows-native-alpha: runner_stubs=skipped cc=%s reason=compiler-not-found\n' "$stub_cc"
fi

"$qstar" help > "$tmp/help.out" 2> "$tmp/help.err"
contains "$tmp/help.out" "build [label]"
contains "$tmp/help.out" "qstar docs"

"$qstar" --file "$corpus/qstar.lua" check > "$tmp/check.out" 2> "$tmp/check.err"
contains "$tmp/check.out" "status ok"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang dry-run \
	//:windows_app > "$tmp/windows-app-dry.out" 2> "$tmp/windows-app-dry.err"
contains "$tmp/windows-app-dry.out" "response_style=msvc"
contains "$tmp/windows-app-dry.out" "/link"
contains "$tmp/windows-app-dry.out" "/LIBPATH:sdk/lib/um/x64"
contains "$tmp/windows-app-dry.out" "kernel32.lib"
contains "$tmp/windows-app-dry.out" "output=build/qstar/out/___windows_app/windows_app.exe"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang dry-run \
	//:windows_mapped > "$tmp/windows-mapped-dry.out" 2> "$tmp/windows-mapped-dry.err"
contains "$tmp/windows-mapped-dry.out" "response_style=msvc"
contains "$tmp/windows-mapped-dry.out" "output=build/qstar/out/___windows_mapped/mapped_named.exe"

printf 'qstar-windows-native-alpha: passed\n'
