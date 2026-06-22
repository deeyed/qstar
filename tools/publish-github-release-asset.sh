#!/bin/sh
set -eu

fail() {
	printf 'qstar-release-upload: %s\n' "$1" >&2
	exit 1
}

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"

version=$(sed -n 's/^#define QSTAR_VERSION "\(.*\)"/\1/p' include/qstar/qstar.h)
test -n "$version" || fail "could not read QSTAR_VERSION from include/qstar/qstar.h"

platform=${QSTAR_RELEASE_PLATFORM:-}
test -n "$platform" || fail "QSTAR_RELEASE_PLATFORM is required"

tag=${QSTAR_RELEASE_TAG:-v$version}
repo=${QSTAR_RELEASE_REPO:-deeyed/qstar}
dist=${QSTAR_RELEASE_DIST:-dist/release}
case "$dist" in
	/*) dist_abs=$dist ;;
	*) dist_abs=$root/$dist ;;
esac
sha_file=$dist_abs/SHA256SUMS
merged_sha=$dist_abs/SHA256SUMS.merged

test "$tag" = "v$version" || \
	fail "QSTAR_RELEASE_TAG '$tag' does not match runtime version 'v$version'"

host=$(uname -s)
arch=$(uname -m)
case "$host" in
	MINGW*|MSYS*|CYGWIN*)
		host_family=windows
		;;
	*)
		host_family=$host
		;;
esac

case "$platform" in
	linux-x86_64)
		archive_ext=tar.gz
		test "$host" = Linux || \
			fail "linux-x86_64 release asset must be published from a Linux host"
		case "$arch" in
		x86_64|amd64) ;;
		*) fail "linux-x86_64 release asset must be published from an x86_64 Linux host" ;;
		esac
		;;
	macos-arm64)
		archive_ext=tar.gz
		test "$host" = Darwin || \
			fail "macos-arm64 release asset must be published from a Darwin host"
		test "$arch" = arm64 || \
			fail "macos-arm64 release asset must be published from an arm64 Darwin host"
		;;
	windows-x86_64)
		archive_ext=zip
		test "$host_family" = windows || \
			fail "windows-x86_64 release asset must be published from a Windows/MSYS2 host"
		case "$arch" in
		x86_64|amd64) ;;
		*) fail "windows-x86_64 release asset must be published from an x86_64 Windows host" ;;
		esac
		;;
	*)
		fail "unsupported release platform '$platform'"
		;;
esac

asset_base=qstar-$tag-$platform.$archive_ext
archive=$dist_abs/$asset_base

command -v gh >/dev/null 2>&1 || fail "gh is required to publish a GitHub release asset"
command -v awk >/dev/null 2>&1 || fail "awk is required to merge SHA256SUMS"

QSTAR_RELEASE_PLATFORM=$platform QSTAR_RELEASE_TAG=$tag QSTAR_RELEASE_DIST=$dist \
	"$root/tools/package-public-beta.sh"

test -f "$archive" || fail "release archive was not created: $archive"
test -f "$sha_file" || fail "release checksum file was not created: $sha_file"
awk -v asset="$asset_base" '
	{
		name = $2
		sub(/^\*/, "", name)
		if (name == asset) {
			found = 1
		}
	}
	END {
		exit(found ? 0 : 1)
	}
' "$sha_file" >/dev/null || \
	fail "local SHA256SUMS does not mention '$asset_base'"

tmp=${TMPDIR:-/tmp}/qstar-release-upload.$$
rm -rf "$tmp"
mkdir -p "$tmp"
cleanup_tmp() {
	rc=$?
	rm -rf "$tmp"
	exit "$rc"
}
trap cleanup_tmp EXIT HUP INT TERM

remote_sha=$tmp/SHA256SUMS.remote
local_sha=$tmp/SHA256SUMS.local
if gh release download "$tag" --repo "$repo" --pattern SHA256SUMS --dir "$tmp" >/dev/null 2>&1; then
	mv "$tmp/SHA256SUMS" "$remote_sha"
else
	: > "$remote_sha"
fi

awk -v asset="$asset_base" '
	{
		name = $2
		sub(/^\*/, "", name)
		if (name != asset) {
			print
		}
	}
' "$remote_sha" > "$merged_sha"
awk -v asset="$asset_base" '
	{
		name = $2
		sub(/^\*/, "", name)
		if (name == asset) {
			print $1 "  " asset
			found = 1
		}
	}
	END {
		exit(found ? 0 : 1)
	}
' "$sha_file" > "$local_sha" || fail "local SHA256SUMS does not mention '$asset_base'"
cat "$local_sha" >> "$merged_sha"
grep -F "$asset_base" "$merged_sha" >/dev/null || \
	fail "merged SHA256SUMS does not mention '$asset_base'"
mv "$merged_sha" "$sha_file"

gh release upload "$tag" "$archive" "$sha_file" --repo "$repo" --clobber

printf 'qstar-release-upload: tag=%s\n' "$tag"
printf 'qstar-release-upload: platform=%s\n' "$platform"
printf 'qstar-release-upload: repo=%s\n' "$repo"
printf 'qstar-release-upload: asset=%s\n' "$archive"
printf 'qstar-release-upload: sha256sums=%s\n' "$sha_file"
printf 'qstar-release-upload: status=ok\n'
