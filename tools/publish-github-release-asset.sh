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
asset_base=qstar-$tag-$platform.tar.gz
dist=${QSTAR_RELEASE_DIST:-dist/release}
case "$dist" in
	/*) dist_abs=$dist ;;
	*) dist_abs=$root/$dist ;;
esac
archive=$dist_abs/$asset_base
sha_file=$dist_abs/SHA256SUMS
merged_sha=$dist_abs/SHA256SUMS.merged

test "$tag" = "v$version" || \
	fail "QSTAR_RELEASE_TAG '$tag' does not match runtime version 'v$version'"

case "$platform" in
	linux-x86_64)
		test "$(uname -s)" = Linux || \
			fail "linux-x86_64 release asset must be published from a Linux host"
		case "$(uname -m)" in
		x86_64|amd64) ;;
		*) fail "linux-x86_64 release asset must be published from an x86_64 Linux host" ;;
		esac
		;;
	macos-arm64)
		test "$(uname -s)" = Darwin || \
			fail "macos-arm64 release asset must be published from a Darwin host"
		test "$(uname -m)" = arm64 || \
			fail "macos-arm64 release asset must be published from an arm64 Darwin host"
		;;
	*)
		fail "unsupported release platform '$platform'"
		;;
esac

command -v gh >/dev/null 2>&1 || fail "gh is required to publish a GitHub release asset"
command -v awk >/dev/null 2>&1 || fail "awk is required to merge SHA256SUMS"

QSTAR_RELEASE_PLATFORM=$platform QSTAR_RELEASE_TAG=$tag QSTAR_RELEASE_DIST=$dist \
	"$root/tools/package-public-beta.sh"

test -f "$archive" || fail "release archive was not created: $archive"
test -f "$sha_file" || fail "release checksum file was not created: $sha_file"
grep -F "$asset_base" "$sha_file" >/dev/null || \
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
if gh release download "$tag" --repo "$repo" --pattern SHA256SUMS --dir "$tmp" >/dev/null 2>&1; then
	mv "$tmp/SHA256SUMS" "$remote_sha"
else
	: > "$remote_sha"
fi

awk -v asset="$asset_base" '$2 != asset { print }' "$remote_sha" > "$merged_sha"
awk -v asset="$asset_base" '$2 == asset { print }' "$sha_file" >> "$merged_sha"
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
