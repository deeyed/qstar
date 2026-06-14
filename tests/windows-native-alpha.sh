#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-windows-native-alpha.$$
corpus=tests/corpus/response-files

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
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

printf 'qstar-windows-native-alpha: host=%s mode=%s\n' "$host" "$mode"

"$qstar" --version > "$tmp/version.out" 2> "$tmp/version.err"
contains "$tmp/version.out" "qstar "

stub_cc=${CC:-cc}
if command -v "$stub_cc" >/dev/null 2>&1; then
	"$stub_cc" -D_WIN32 -std=c99 -Wall -Wextra -Wpedantic \
		-Iinclude -Ivendor/lua -c src/daemon.c -o "$tmp/daemon-win-stub.o"
	test -s "$tmp/daemon-win-stub.o" ||
		fail "Windows daemon stub object was not created"
	printf 'qstar-windows-native-alpha: daemon_stub=compiled cc=%s\n' "$stub_cc"
else
	printf 'qstar-windows-native-alpha: daemon_stub=skipped cc=%s reason=compiler-not-found\n' "$stub_cc"
fi

"$qstar" help > "$tmp/help.out" 2> "$tmp/help.err"
contains "$tmp/help.out" "build [label]"
contains "$tmp/help.out" "qstar docs"

"$qstar" --file "$corpus/qstar.lua" check > "$tmp/check.out" 2> "$tmp/check.err"
contains "$tmp/check.out" "status ok"

"$qstar" --file "$corpus/qstar.lua" --profile windows-msvc dry-run \
	//:windows_app > "$tmp/windows-app-dry.out" 2> "$tmp/windows-app-dry.err"
contains "$tmp/windows-app-dry.out" "response_style=msvc"
contains "$tmp/windows-app-dry.out" "/link"
contains "$tmp/windows-app-dry.out" "/LIBPATH:sdk/lib/um/x64"
contains "$tmp/windows-app-dry.out" "kernel32.lib"
contains "$tmp/windows-app-dry.out" "output=build/qstar/out/___windows_app/windows_app.exe"

"$qstar" --file "$corpus/qstar.lua" --profile windows-msvc-artifact-map dry-run \
	//:windows_mapped > "$tmp/windows-mapped-dry.out" 2> "$tmp/windows-mapped-dry.err"
contains "$tmp/windows-mapped-dry.out" "response_style=msvc"
contains "$tmp/windows-mapped-dry.out" "output=build/qstar/out/___windows_mapped/profile_named.exe"

printf 'qstar-windows-native-alpha: passed\n'
