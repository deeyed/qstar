#!/bin/sh
set -eu

tmp=${TMPDIR:-/tmp}/qstar-windows-release-package.$$
out=$tmp/package.out
err=$tmp/package.err
dist=$tmp/release
version=$(sed -n 's/^#define QSTAR_VERSION "\(.*\)"/\1/p' include/qstar/qstar.h)

fail() {
	printf 'qstar-windows-release-package: %s\n' "$1" >&2
	exit 1
}

contains() {
	file=$1
	pattern=$2
	grep -F -q -- "$pattern" "$file" ||
		fail "missing pattern '$pattern' in $file"
}

cleanup() {
	rc=$?
	rm -rf "$tmp"
	exit "$rc"
}

trap cleanup EXIT HUP INT TERM
rm -rf "$tmp"
mkdir -p "$tmp"

test -n "$version" || fail "could not read QSTAR_VERSION"

QSTAR_RELEASE_PLATFORM=windows-x86_64 \
QSTAR_RELEASE_DRY_RUN=1 \
QSTAR_RELEASE_DIST="$dist" \
	sh tools/package-public-beta.sh > "$out" 2> "$err"

contains "$out" "qstar-release-package: platform=windows-x86_64"
contains "$out" "qstar-release-package: mode=dry-run"
contains "$out" "qstar-release-package: archive_format=zip"
contains "$out" "qstar-v$version-windows-x86_64.zip"

plan=$dist/qstar-v$version-windows-x86_64.package-plan.txt
expected=$dist/qstar-v$version-windows-x86_64.expected-contents.txt
test -f "$plan" || fail "missing Windows release package plan"
test -f "$expected" || fail "missing Windows release expected contents"

contains "$plan" "qstar-release-package-plan v1"
contains "$plan" "platform=windows-x86_64"
contains "$plan" "mode=dry-run"
contains "$plan" "asset=qstar-v$version-windows-x86_64.zip"
contains "$plan" "archive_format=zip"
contains "$plan" "binary=bin/qstar.exe"
contains "$plan" "docs=share/doc/qstar/wiki"
contains "$plan" "man=share/man/man1/qstar.1,share/man/man5/qstar-lua.5"
contains "$plan" "providers=share/qstar/languages/zig,share/qstar/languages/rust,share/qstar/languages/cuda"

contains "$expected" "bin/qstar.exe"
contains "$expected" "share/doc/qstar/wiki/AI_INDEX.md"
contains "$expected" "share/doc/qstar/wiki/reference/qstar-lua.md"
contains "$expected" "share/qstar/languages/zig/zig.qsm"
contains "$expected" "share/qstar/languages/rust/provider.lua"
contains "$expected" "share/qstar/languages/cuda/cuda.qsm"
contains "$expected" "share/man/man1/qstar.1"
contains "$expected" "share/man/man5/qstar-lua.5"
contains "$expected" "README.md"
contains "$expected" "LICENSE/lua.txt"

printf 'qstar-windows-release-package: passed\n'
