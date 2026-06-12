#!/bin/sh
set -eu

fail() {
	printf 'qstar-release-package: %s\n' "$1" >&2
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

expected_version="qstar $version"
tag=$(git describe --tags --exact-match 2>/dev/null || true)
if test -n "$tag" && test "$tag" != "v$version"; then
	fail "current tag '$tag' does not match runtime version 'v$version'"
fi
if test -n "${QSTAR_RELEASE_TAG:-}" && test "$QSTAR_RELEASE_TAG" != "v$version"; then
	fail "QSTAR_RELEASE_TAG '$QSTAR_RELEASE_TAG' does not match runtime version 'v$version'"
fi

dist=${QSTAR_RELEASE_DIST:-dist/release}
case "$dist" in
	/*) dist_abs=$dist ;;
	*) dist_abs=$root/$dist ;;
esac

name=qstar-v$version-$platform
install_root=$dist_abs/$name-root
archive_base=$name.tar.gz
archive=$dist_abs/$archive_base
sha_file=$dist_abs/SHA256SUMS
contents_file=$dist_abs/$name.contents.txt

case "$install_root" in
	"$root"/dist/release/qstar-v*-root) ;;
	*) fail "refusing to clean unexpected install root '$install_root'" ;;
esac

mkdir -p "$dist_abs"
rm -rf "$install_root"
rm -f "$archive" "$sha_file" "$contents_file"

make_cmd=${MAKE:-make}
$make_cmd install PREFIX="$install_root"

for required in README.md README.ko.md LICENSE.md LICENSE/lua.txt LICENSE/README.md; do
	test -f "$required" || fail "missing release source file '$required'"
done

cp README.md "$install_root/README.md"
cp README.ko.md "$install_root/README.ko.md"
cp LICENSE.md "$install_root/LICENSE.md"
mkdir -p "$install_root/LICENSE"
cp LICENSE/lua.txt "$install_root/LICENSE/lua.txt"
cp LICENSE/README.md "$install_root/LICENSE/README.md"

actual_version=$("$install_root/bin/qstar" --version)
test "$actual_version" = "$expected_version" || \
	fail "installed qstar version '$actual_version' does not match '$expected_version'"

test -f "$install_root/share/doc/qstar/wiki/AI_INDEX.md" || fail "installed wiki AI_INDEX.md missing"
test -f "$install_root/share/man/man1/qstar.1" || fail "installed qstar(1) manpage missing"
test -f "$install_root/share/man/man5/qstar-lua.5" || fail "installed qstar-lua(5) manpage missing"

docs_path=$(QSTAR_DOC_DIR="$install_root/share/doc/qstar" "$install_root/bin/qstar" docs --path)
case "$docs_path" in
	"$install_root"/share/doc/qstar/wiki) ;;
	*) fail "docs --path returned '$docs_path'" ;;
esac

if test "$host" = Darwin && command -v codesign >/dev/null 2>&1; then
	codesign -dv --verbose=2 "$install_root/bin/qstar" > "$dist_abs/codesign.txt" 2>&1 || \
		fail "codesign detail verification failed"
	codesign --verify "$install_root/bin/qstar" >/dev/null 2>&1 || \
		fail "codesign signature verification failed"
fi

if test "$platform" = macos-arm64 && command -v file >/dev/null 2>&1; then
	file "$install_root/bin/qstar" | grep -F "arm64" >/dev/null || \
		fail "release binary is not reported as arm64"
fi

(
	cd "$install_root"
	tar -czf "$archive" bin share README.md README.ko.md LICENSE.md LICENSE
)

tar -tzf "$archive" > "$contents_file"
for entry in \
	bin/qstar \
	share/doc/qstar/wiki/AI_INDEX.md \
	share/man/man1/qstar.1 \
	share/man/man5/qstar-lua.5 \
	README.md \
	README.ko.md \
	LICENSE.md \
	LICENSE/lua.txt
do
	grep -Fx "$entry" "$contents_file" >/dev/null || fail "tarball is missing '$entry'"
done

if grep -E '\.vsix$' "$contents_file" >/dev/null; then
	fail "VSCode VSIX must not be included in the public beta runtime tarball"
fi

if command -v shasum >/dev/null 2>&1; then
	(cd "$dist_abs" && shasum -a 256 "$archive_base" > SHA256SUMS)
elif command -v sha256sum >/dev/null 2>&1; then
	(cd "$dist_abs" && sha256sum "$archive_base" > SHA256SUMS)
else
	fail "neither shasum nor sha256sum is available"
fi

grep -F "$archive_base" "$sha_file" >/dev/null || fail "SHA256SUMS does not mention '$archive_base'"

printf 'qstar-release-package: version=%s\n' "$version"
printf 'qstar-release-package: platform=%s\n' "$platform"
printf 'qstar-release-package: asset=%s\n' "$archive"
printf 'qstar-release-package: sha256sums=%s\n' "$sha_file"
if test -n "$tag"; then
	printf 'qstar-release-package: tag=%s\n' "$tag"
else
	printf 'qstar-release-package: tag=not-on-tag\n'
fi
