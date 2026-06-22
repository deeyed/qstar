#!/bin/sh
set -eu

fail() {
	printf 'qstar-release-package: %s\n' "$1" >&2
	exit 1
}

contains_exact_line() {
	file=$1
	line=$2
	grep -Fx "$line" "$file" >/dev/null || fail "archive is missing '$line'"
}

normalize_bool() {
	case "$1" in
		""|0|false|False|FALSE|no|No|NO|off|Off|OFF)
			printf '0\n'
			;;
		1|true|True|TRUE|yes|Yes|YES|on|On|ON)
			printf '1\n'
			;;
		*)
			fail "invalid boolean '$1' for QSTAR_RELEASE_DRY_RUN"
			;;
	esac
}

path_matches() {
	actual=$1
	expected=$2
	if test "$actual" = "$expected"; then
		return 0
	fi
	if command -v cygpath >/dev/null 2>&1; then
		expected_mixed=$(cygpath -m "$expected" 2>/dev/null || true)
		if test -n "$expected_mixed" && test "$actual" = "$expected_mixed"; then
			return 0
		fi
		actual_posix=$(cygpath -u "$actual" 2>/dev/null || true)
		if test -n "$actual_posix" && test "$actual_posix" = "$expected"; then
			return 0
		fi
	fi
	return 1
}

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"

version=$(sed -n 's/^#define QSTAR_VERSION "\(.*\)"/\1/p' include/qstar/qstar.h)
test -n "$version" || fail "could not read QSTAR_VERSION from include/qstar/qstar.h"

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

if test -n "${QSTAR_RELEASE_PLATFORM:-}"; then
	platform=$QSTAR_RELEASE_PLATFORM
elif test "$host" = Darwin && test "$arch" = arm64; then
	platform=macos-arm64
elif test "$host_family" = windows; then
	case "$arch" in
		x86_64|amd64) platform=windows-x86_64 ;;
		*) platform=$(printf 'windows-%s' "$arch" | tr '[:upper:]' '[:lower:]') ;;
	esac
else
	platform=$(printf '%s-%s' "$host" "$arch" | tr '[:upper:]' '[:lower:]')
fi

dry_run=$(normalize_bool "${QSTAR_RELEASE_DRY_RUN:-0}")

case "$platform" in
	macos-arm64)
		if test "$host" != Darwin; then
			fail "macos-arm64 release package must be built on a Darwin host"
		fi
		if test "$arch" != arm64; then
			fail "macos-arm64 release package must be built on an arm64 Darwin host, got '$arch'"
		fi
		archive_format=tar.gz
		archive_ext=tar.gz
		binary_rel=bin/qstar
		;;
	linux-x86_64)
		if test "$host" != Linux; then
			fail "linux-x86_64 release package must be built on a Linux host"
		fi
		case "$arch" in
			x86_64|amd64) ;;
			*) fail "linux-x86_64 release package must be built on an x86_64 Linux host, got '$arch'" ;;
		esac
		archive_format=tar.gz
		archive_ext=tar.gz
		binary_rel=bin/qstar
		;;
	windows-x86_64)
		if test "$dry_run" -eq 0; then
			if test "$host_family" != windows; then
				fail "windows-x86_64 release package must be built on a Windows/MSYS2 host"
			fi
			case "$arch" in
				x86_64|amd64) ;;
				*) fail "windows-x86_64 release package must be built on an x86_64 Windows host, got '$arch'" ;;
			esac
		fi
		archive_format=zip
		archive_ext=zip
		binary_rel=bin/qstar.exe
		;;
	*)
		fail "unsupported release platform '$platform'"
		;;
esac

expected_version="qstar $version"
tag=$(git describe --tags --exact-match 2>/dev/null || true)
dirty=0
if ! git diff --quiet --ignore-submodules -- 2>/dev/null ||
    ! git diff --cached --quiet --ignore-submodules -- 2>/dev/null; then
	dirty=1
fi
if test -n "$tag" && test "$tag" != "v$version" && test "$dirty" -eq 0; then
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
archive_base=$name.$archive_ext
archive=$dist_abs/$archive_base
sha_file=$dist_abs/SHA256SUMS
contents_file=$dist_abs/$name.contents.txt
expected_contents_file=$dist_abs/$name.expected-contents.txt
plan_file=$dist_abs/$name.package-plan.txt
file_report=$dist_abs/file-$platform.txt
ldd_report=$dist_abs/ldd-$platform.txt
docs_show_report=$dist_abs/docs-show-qstar-lua.txt
extract_root=$dist_abs/$name-extract-smoke
extract_file_report=$dist_abs/extract-file-$platform.txt
extract_ldd_report=$dist_abs/extract-ldd-$platform.txt
extract_docs_show_report=$dist_abs/extract-docs-show-qstar-lua.txt

case "$install_root" in
	"$dist_abs"/qstar-v*-root) ;;
	*) fail "refusing to clean unexpected install root '$install_root'" ;;
esac
case "$extract_root" in
	"$dist_abs"/qstar-v*-extract-smoke) ;;
	*) fail "refusing to clean unexpected extract smoke root '$extract_root'" ;;
esac

write_expected_contents() {
	cat > "$expected_contents_file" <<EOF
$binary_rel
share/doc/qstar/wiki/AI_INDEX.md
share/doc/qstar/wiki/README.md
share/doc/qstar/wiki/reference/qstar-lua.md
share/qstar/languages/cuda/cuda.qsm
share/qstar/languages/cuda/provider.lua
share/qstar/languages/rust/rust.qsm
share/qstar/languages/rust/provider.lua
share/qstar/languages/zig/zig.qsm
share/qstar/languages/zig/provider.lua
share/man/man1/qstar.1
share/man/man5/qstar-lua.5
README.md
README.ko.md
LICENSE.md
LICENSE/lua.txt
LICENSE/README.md
EOF
}

write_package_plan() {
	mode=$1
	write_expected_contents
	cat > "$plan_file" <<EOF
qstar-release-package-plan v1
version=$version
platform=$platform
mode=$mode
asset=$archive_base
archive_format=$archive_format
binary=$binary_rel
docs=share/doc/qstar/wiki
man=share/man/man1/qstar.1,share/man/man5/qstar-lua.5
providers=share/qstar/languages/zig,share/qstar/languages/rust,share/qstar/languages/cuda
expected_contents=$expected_contents_file
EOF
}

list_archive() {
	case "$archive_format" in
		tar.gz)
			tar -tzf "$archive"
			;;
		zip)
			if command -v unzip >/dev/null 2>&1; then
				unzip -Z1 "$archive"
			elif command -v zipinfo >/dev/null 2>&1; then
				zipinfo -1 "$archive"
			else
				fail "unzip or zipinfo is required to inspect Windows zip packages"
			fi
			;;
		*)
			fail "unsupported archive format '$archive_format'"
			;;
	esac
}

extract_archive() {
	case "$archive_format" in
		tar.gz)
			tar -xzf "$archive" -C "$extract_root"
			;;
		zip)
			command -v unzip >/dev/null 2>&1 || \
				fail "unzip is required to extract Windows zip packages"
			unzip -q "$archive" -d "$extract_root"
			;;
		*)
			fail "unsupported archive format '$archive_format'"
			;;
	esac
}

mkdir -p "$dist_abs"
rm -rf "$install_root" "$extract_root"
rm -f "$archive" "$sha_file" "$contents_file" "$expected_contents_file" \
	"$plan_file" "$file_report" "$ldd_report" "$docs_show_report" \
	"$extract_file_report" "$extract_ldd_report" "$extract_docs_show_report"

if test "$dry_run" -ne 0; then
	write_package_plan dry-run
	printf 'qstar-release-package: version=%s\n' "$version"
	printf 'qstar-release-package: platform=%s\n' "$platform"
	printf 'qstar-release-package: mode=dry-run\n'
	printf 'qstar-release-package: archive_format=%s\n' "$archive_format"
	printf 'qstar-release-package: asset=%s\n' "$archive"
	printf 'qstar-release-package: plan=%s\n' "$plan_file"
	if test -n "$tag" && test "$dirty" -ne 0 && test "$tag" != "v$version"; then
		printf 'qstar-release-package: tag=dirty-on-tag:%s\n' "$tag"
	elif test -n "$tag"; then
		printf 'qstar-release-package: tag=%s\n' "$tag"
	else
		printf 'qstar-release-package: tag=not-on-tag\n'
	fi
	exit 0
fi

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

installed_bin=$install_root/$binary_rel
if test "$platform" = windows-x86_64; then
	if test ! -f "$installed_bin"; then
		if test -f "$install_root/bin/qstar"; then
			cp "$install_root/bin/qstar" "$installed_bin"
			chmod +x "$installed_bin" 2>/dev/null || true
			rm -f "$install_root/bin/qstar"
		else
			fail "installed Windows binary '$binary_rel' missing"
		fi
	fi
else
	test -x "$installed_bin" || fail "installed qstar binary missing"
fi

actual_version=$("$installed_bin" --version)
test "$actual_version" = "$expected_version" || \
	fail "installed qstar version '$actual_version' does not match '$expected_version'"

test -f "$install_root/share/doc/qstar/wiki/AI_INDEX.md" || fail "installed wiki AI_INDEX.md missing"
test -f "$install_root/share/doc/qstar/wiki/README.md" || fail "installed wiki README.md missing"
test -f "$install_root/share/doc/qstar/wiki/reference/qstar-lua.md" || \
	fail "installed wiki reference/qstar-lua.md missing"
test -f "$install_root/share/qstar/languages/zig/zig.qsm" || \
	fail "installed Zig language provider manifest missing"
test -f "$install_root/share/qstar/languages/zig/provider.lua" || \
	fail "installed Zig language provider implementation missing"
test -f "$install_root/share/qstar/languages/rust/rust.qsm" || \
	fail "installed Rust language provider manifest missing"
test -f "$install_root/share/qstar/languages/rust/provider.lua" || \
	fail "installed Rust language provider implementation missing"
test -f "$install_root/share/qstar/languages/cuda/cuda.qsm" || \
	fail "installed CUDA language provider manifest missing"
test -f "$install_root/share/qstar/languages/cuda/provider.lua" || \
	fail "installed CUDA language provider implementation missing"
test -s "$install_root/share/man/man1/qstar.1" || fail "installed qstar(1) manpage missing"
test -s "$install_root/share/man/man5/qstar-lua.5" || fail "installed qstar-lua(5) manpage missing"

docs_path=$(QSTAR_DOC_DIR="$install_root/share/doc/qstar" "$installed_bin" docs --path)
expected_docs_path=$install_root/share/doc/qstar/wiki
path_matches "$docs_path" "$expected_docs_path" || fail "docs --path returned '$docs_path'"
ai_path=$(QSTAR_DOC_DIR="$install_root/share/doc/qstar" "$installed_bin" docs --ai-index)
expected_ai_path=$install_root/share/doc/qstar/wiki/AI_INDEX.md
path_matches "$ai_path" "$expected_ai_path" || fail "docs --ai-index returned '$ai_path'"
QSTAR_DOC_DIR="$install_root/share/doc/qstar" \
	"$installed_bin" docs --show reference/qstar-lua.md > "$docs_show_report"
grep -F "qstar.project" "$docs_show_report" >/dev/null || \
	fail "docs --show reference/qstar-lua.md did not print qstar-lua reference"

if test "$host" = Darwin && command -v codesign >/dev/null 2>&1; then
	codesign -dv --verbose=2 "$installed_bin" > "$dist_abs/codesign.txt" 2>&1 || \
		fail "codesign detail verification failed"
	codesign --verify "$installed_bin" >/dev/null 2>&1 || \
		fail "codesign signature verification failed"
fi

if command -v file >/dev/null 2>&1; then
	file "$installed_bin" > "$file_report"
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
	windows-x86_64)
		grep -E "PE32\\+|x86[-_ ]?64|x86-64|AMD64" "$file_report" >/dev/null || \
			fail "windows release binary is not reported as x86-64 PE"
		;;
	esac
else
	case "$platform" in
	macos-arm64|linux-x86_64|windows-x86_64)
		fail "file(1) is required for $platform release sanity"
		;;
	esac
fi

if test "$platform" = linux-x86_64; then
	command -v ldd >/dev/null 2>&1 || \
		fail "ldd is required for linux-x86_64 release sanity"
	if ! ldd "$installed_bin" > "$ldd_report" 2>&1; then
		if grep -F "statically linked" "$file_report" >/dev/null; then
			printf 'statically linked binary; ldd is not applicable\n' > "$ldd_report"
		else
			fail "ldd failed for linux-x86_64 release binary"
		fi
	fi
fi

case "$archive_format" in
	tar.gz)
		(
			cd "$install_root"
			tar -czf "$archive" bin share README.md README.ko.md LICENSE.md LICENSE
		)
		;;
	zip)
		command -v zip >/dev/null 2>&1 || \
			fail "zip is required to create Windows release packages"
		(
			cd "$install_root"
			zip -qr "$archive" bin share README.md README.ko.md LICENSE.md LICENSE
		)
		;;
	*)
		fail "unsupported archive format '$archive_format'"
		;;
esac

write_package_plan package
list_archive > "$contents_file"
while IFS= read -r entry; do
	contains_exact_line "$contents_file" "$entry"
done < "$expected_contents_file"

if grep -E '\.vsix$' "$contents_file" >/dev/null; then
	fail "VSCode VSIX must not be included in the public beta runtime package"
fi

if command -v shasum >/dev/null 2>&1; then
	(cd "$dist_abs" && shasum -a 256 "$archive_base" > SHA256SUMS)
elif command -v sha256sum >/dev/null 2>&1; then
	(cd "$dist_abs" && sha256sum "$archive_base" > SHA256SUMS)
else
	fail "neither shasum nor sha256sum is available"
fi

grep -F "$archive_base" "$sha_file" >/dev/null || fail "SHA256SUMS does not mention '$archive_base'"

mkdir -p "$extract_root"
extract_archive
extract_bin=$extract_root/$binary_rel
if test "$platform" = windows-x86_64; then
	test -f "$extract_bin" || fail "extracted qstar.exe binary missing"
else
	test -x "$extract_bin" || fail "extracted qstar binary missing"
fi
extract_version=$("$extract_bin" --version)
test "$extract_version" = "$expected_version" || \
	fail "extracted qstar version '$extract_version' does not match '$expected_version'"
extract_docs_path=$(QSTAR_DOC_DIR="$extract_root/share/doc/qstar" "$extract_bin" docs --path)
expected_extract_docs_path=$extract_root/share/doc/qstar/wiki
path_matches "$extract_docs_path" "$expected_extract_docs_path" || \
	fail "extracted docs --path returned '$extract_docs_path'"
extract_ai_path=$(QSTAR_DOC_DIR="$extract_root/share/doc/qstar" "$extract_bin" docs --ai-index)
expected_extract_ai_path=$extract_root/share/doc/qstar/wiki/AI_INDEX.md
path_matches "$extract_ai_path" "$expected_extract_ai_path" || \
	fail "extracted docs --ai-index returned '$extract_ai_path'"
QSTAR_DOC_DIR="$extract_root/share/doc/qstar" \
	"$extract_bin" docs --show reference/qstar-lua.md > "$extract_docs_show_report"
grep -F "qstar.project" "$extract_docs_show_report" >/dev/null || \
	fail "extracted docs --show reference/qstar-lua.md did not print qstar-lua reference"
test -s "$extract_root/share/man/man1/qstar.1" || fail "extracted qstar(1) manpage missing"
test -s "$extract_root/share/man/man5/qstar-lua.5" || fail "extracted qstar-lua(5) manpage missing"
test -f "$extract_root/share/qstar/languages/zig/zig.qsm" || \
	fail "extracted Zig language provider manifest missing"
test -f "$extract_root/share/qstar/languages/zig/provider.lua" || \
	fail "extracted Zig language provider implementation missing"
test -f "$extract_root/share/qstar/languages/rust/rust.qsm" || \
	fail "extracted Rust language provider manifest missing"
test -f "$extract_root/share/qstar/languages/rust/provider.lua" || \
	fail "extracted Rust language provider implementation missing"
test -f "$extract_root/share/qstar/languages/cuda/cuda.qsm" || \
	fail "extracted CUDA language provider manifest missing"
test -f "$extract_root/share/qstar/languages/cuda/provider.lua" || \
	fail "extracted CUDA language provider implementation missing"

if command -v file >/dev/null 2>&1; then
	file "$extract_bin" > "$extract_file_report"
	case "$platform" in
	macos-arm64)
		grep -F "arm64" "$extract_file_report" >/dev/null || \
			fail "extracted release binary is not reported as arm64"
		;;
	linux-x86_64)
		grep -F "ELF" "$extract_file_report" >/dev/null || \
			fail "extracted linux release binary is not reported as ELF"
		grep -E "x86[-_]64|x86-64|AMD x86-64" "$extract_file_report" >/dev/null || \
			fail "extracted linux release binary is not reported as x86-64"
		;;
	windows-x86_64)
		grep -E "PE32\\+|x86[-_ ]?64|x86-64|AMD64" "$extract_file_report" >/dev/null || \
			fail "extracted windows release binary is not reported as x86-64 PE"
		;;
	esac
fi

if test "$platform" = linux-x86_64; then
	if ! ldd "$extract_bin" > "$extract_ldd_report" 2>&1; then
		if grep -F "statically linked" "$extract_file_report" >/dev/null; then
			printf 'statically linked binary; ldd is not applicable\n' > "$extract_ldd_report"
		else
			fail "ldd failed for extracted linux-x86_64 release binary"
		fi
	fi
fi

printf 'qstar-release-package: version=%s\n' "$version"
printf 'qstar-release-package: platform=%s\n' "$platform"
printf 'qstar-release-package: mode=package\n'
printf 'qstar-release-package: archive_format=%s\n' "$archive_format"
printf 'qstar-release-package: asset=%s\n' "$archive"
printf 'qstar-release-package: sha256sums=%s\n' "$sha_file"
printf 'qstar-release-package: plan=%s\n' "$plan_file"
printf 'qstar-release-package: extract_smoke=%s\n' "$extract_root"
if test -n "$tag" && test "$dirty" -ne 0 && test "$tag" != "v$version"; then
	printf 'qstar-release-package: tag=dirty-on-tag:%s\n' "$tag"
elif test -n "$tag"; then
	printf 'qstar-release-package: tag=%s\n' "$tag"
else
	printf 'qstar-release-package: tag=not-on-tag\n'
fi
