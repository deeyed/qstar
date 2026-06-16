#!/bin/sh
set -eu

fail() {
	printf 'qstar-release-download-smoke: %s\n' "$1" >&2
	exit 1
}

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"

version=$(sed -n 's/^#define QSTAR_VERSION "\(.*\)"/\1/p' include/qstar/qstar.h)
test -n "$version" || fail "could not read QSTAR_VERSION from include/qstar/qstar.h"

host=$(uname -s)
arch=$(uname -m)
if test -n "${QSTAR_RELEASE_PLATFORM:-}"; then
	platform=$QSTAR_RELEASE_PLATFORM
elif test "$host" = Darwin && test "$arch" = arm64; then
	platform=macos-arm64
else
	platform=$(printf '%s-%s' "$host" "$arch" | tr '[:upper:]' '[:lower:]')
fi

tag=${QSTAR_RELEASE_TAG:-v$version}
release_version=${tag#v}
asset=qstar-$tag-$platform.tar.gz
expected_version="qstar $release_version"
base_url=${QSTAR_RELEASE_URL_BASE:-https://github.com/deeyed/qstar/releases/download/$tag}

case "$platform" in
	macos-arm64|linux-x86_64) ;;
	*) fail "download smoke is only defined for macos-arm64 or linux-x86_64, got '$platform'" ;;
esac
if test "$platform" = linux-x86_64 && test "$host" != Linux; then
	fail "linux-x86_64 download smoke must be run on a Linux host"
fi
if test "$platform" = macos-arm64 && test "$host" != Darwin; then
	fail "macos-arm64 download smoke must be run on a Darwin host"
fi

command -v curl >/dev/null 2>&1 || fail "curl is required for GitHub release download smoke"
command -v tar >/dev/null 2>&1 || fail "tar is required for GitHub release download smoke"
command -v awk >/dev/null 2>&1 || fail "awk is required for checksum verification"

if test -n "${QSTAR_RELEASE_SMOKE_DIR:-}"; then
	tmp=$QSTAR_RELEASE_SMOKE_DIR
	cleanup=0
else
	tmp=${TMPDIR:-/tmp}/qstar-release-download-smoke.$$
	cleanup=1
fi

case "$tmp" in
	/*) ;;
	*) fail "QSTAR_RELEASE_SMOKE_DIR must be absolute" ;;
esac

case "$tmp" in
	/tmp/qstar-release-download-smoke.*|/var/*/qstar-release-download-smoke.*|/private/var/*/qstar-release-download-smoke.*) ;;
	*)
		if test "$cleanup" -ne 0; then
			fail "refusing to cleanup unexpected smoke dir '$tmp'"
		fi
		;;
esac

if test "$cleanup" -ne 0; then
	rm -rf "$tmp"
else
	rm -rf "$tmp/download" "$tmp/root"
fi
mkdir -p "$tmp/download" "$tmp/root"

cleanup_tmp() {
	rc=$?
	if test "$cleanup" -ne 0; then
		rm -rf "$tmp"
	fi
	exit "$rc"
}
trap cleanup_tmp EXIT HUP INT TERM

download=$tmp/download
install_root=$tmp/root
archive=$download/$asset
sha_file=$download/SHA256SUMS
contents=$download/contents.txt
file_report=$download/file-$platform.txt
ldd_report=$download/ldd-$platform.txt
docs_home_report=$download/docs-show-home.txt
docs_lua_report=$download/docs-show-qstar-lua.txt
man1_report=$download/man-qstar.1.txt
man5_report=$download/man-qstar-lua.5.txt

curl -fsSL "$base_url/$asset" -o "$archive"
curl -fsSL "$base_url/SHA256SUMS" -o "$sha_file"

expected_sha=$(awk -v file="$asset" '$2 == file { print $1 }' "$sha_file")
test -n "$expected_sha" || fail "SHA256SUMS does not mention '$asset'"

if command -v shasum >/dev/null 2>&1; then
	actual_sha=$(shasum -a 256 "$archive" | awk '{ print $1 }')
elif command -v sha256sum >/dev/null 2>&1; then
	actual_sha=$(sha256sum "$archive" | awk '{ print $1 }')
else
	fail "neither shasum nor sha256sum is available"
fi
test "$actual_sha" = "$expected_sha" || \
	fail "checksum mismatch for '$asset': expected '$expected_sha', got '$actual_sha'"

tar -tzf "$archive" > "$contents"
for entry in \
	bin/qstar \
	share/doc/qstar/wiki/AI_INDEX.md \
	share/doc/qstar/wiki/README.md \
	share/doc/qstar/wiki/reference/qstar-lua.md \
	share/qstar/languages/zig/zig.qsm \
	share/qstar/languages/zig/provider.lua \
	share/man/man1/qstar.1 \
	share/man/man5/qstar-lua.5 \
	README.md \
	README.ko.md \
	LICENSE.md \
	LICENSE/lua.txt \
	LICENSE/README.md
do
	grep -Fx "$entry" "$contents" >/dev/null || fail "tarball is missing '$entry'"
done
if grep -E '\.vsix$' "$contents" >/dev/null; then
	fail "VSCode VSIX must not be included in the public beta runtime tarball"
fi

tar -xzf "$archive" -C "$install_root"
actual_version=$("$install_root/bin/qstar" --version)
test "$actual_version" = "$expected_version" || \
	fail "installed qstar version '$actual_version' does not match '$expected_version'"

docs_path=$(QSTAR_DOC_DIR="$install_root/share/doc/qstar" "$install_root/bin/qstar" docs --path)
case "$docs_path" in
	"$install_root"/share/doc/qstar/wiki) ;;
	*) fail "docs --path returned '$docs_path'" ;;
esac
ai_path=$(QSTAR_DOC_DIR="$install_root/share/doc/qstar" "$install_root/bin/qstar" docs --ai-index)
case "$ai_path" in
	"$install_root"/share/doc/qstar/wiki/AI_INDEX.md) ;;
	*) fail "docs --ai-index returned '$ai_path'" ;;
esac
QSTAR_DOC_DIR="$install_root/share/doc/qstar" \
	"$install_root/bin/qstar" docs --show README.md > "$docs_home_report"
grep -F "QStar" "$docs_home_report" >/dev/null || \
	fail "docs --show README.md did not print wiki home"
QSTAR_DOC_DIR="$install_root/share/doc/qstar" \
	"$install_root/bin/qstar" docs --show reference/qstar-lua.md > "$docs_lua_report"
grep -F "qstar.project" "$docs_lua_report" >/dev/null || \
	fail "docs --show reference/qstar-lua.md did not print qstar-lua reference"

test -s "$install_root/share/man/man1/qstar.1" || fail "installed qstar(1) manpage missing"
test -s "$install_root/share/man/man5/qstar-lua.5" || fail "installed qstar-lua(5) manpage missing"
test -f "$install_root/share/qstar/languages/zig/zig.qsm" || \
	fail "installed Zig language provider manifest missing"
test -f "$install_root/share/qstar/languages/zig/provider.lua" || \
	fail "installed Zig language provider implementation missing"
grep -F ".Dt QSTAR 1" "$install_root/share/man/man1/qstar.1" >/dev/null || \
	fail "installed qstar(1) manpage does not look like qstar"
grep -F ".Dt QSTAR-LUA 5" "$install_root/share/man/man5/qstar-lua.5" >/dev/null || \
	fail "installed qstar-lua(5) manpage does not look like qstar-lua"
if command -v man >/dev/null 2>&1; then
	MANPAGER=cat MANWIDTH=80 man -l "$install_root/share/man/man1/qstar.1" > "$man1_report" 2>/dev/null || true
	MANPAGER=cat MANWIDTH=80 man -l "$install_root/share/man/man5/qstar-lua.5" > "$man5_report" 2>/dev/null || true
fi

if command -v file >/dev/null 2>&1; then
	file "$install_root/bin/qstar" > "$file_report"
	case "$platform" in
	macos-arm64)
		grep -F "arm64" "$file_report" >/dev/null || \
			fail "release binary is not reported as arm64"
		;;
	linux-x86_64)
		grep -F "ELF" "$file_report" >/dev/null || \
			fail "linux release binary is not reported as ELF"
		grep -E "x86[-_]64|x86-64|AMD x86-64" "$file_report" >/dev/null || \
			fail "linux release binary is not reported as x86-64"
		;;
	esac
else
	case "$platform" in
	macos-arm64|linux-x86_64)
		fail "file(1) is required for $platform download smoke"
		;;
	esac
fi

if test "$platform" = linux-x86_64; then
	command -v ldd >/dev/null 2>&1 || fail "ldd is required for linux-x86_64 download smoke"
	if ! ldd "$install_root/bin/qstar" > "$ldd_report" 2>&1; then
		if grep -F "statically linked" "$file_report" >/dev/null; then
			printf 'statically linked binary; ldd is not applicable\n' > "$ldd_report"
		else
			fail "ldd failed for downloaded linux-x86_64 release binary"
		fi
	fi
fi

if test "$host" = Darwin && command -v codesign >/dev/null 2>&1; then
	codesign -dv --verbose=2 "$install_root/bin/qstar" > "$download/codesign.txt" 2>&1 || \
		fail "codesign detail verification failed"
	codesign --verify "$install_root/bin/qstar" >/dev/null 2>&1 || \
		fail "codesign signature verification failed"
fi

printf 'qstar-release-download-smoke: tag=%s\n' "$tag"
printf 'qstar-release-download-smoke: platform=%s\n' "$platform"
printf 'qstar-release-download-smoke: asset=%s\n' "$asset"
printf 'qstar-release-download-smoke: sha256=%s\n' "$actual_sha"
printf 'qstar-release-download-smoke: installed_version=%s\n' "$actual_version"
printf 'qstar-release-download-smoke: status=ok\n'
