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

tag=${QSTAR_RELEASE_TAG:-v$version}
release_version=${tag#v}
expected_version="qstar $release_version"
base_url=${QSTAR_RELEASE_URL_BASE:-https://github.com/deeyed/qstar/releases/download/$tag}

case "$platform" in
	macos-arm64|linux-x86_64)
		archive_format=tar.gz
		archive_ext=tar.gz
		binary_rel=bin/qstar
		;;
	windows-x86_64)
		archive_format=zip
		archive_ext=zip
		binary_rel=bin/qstar.exe
		;;
	*) fail "download smoke is only defined for macos-arm64, linux-x86_64, or windows-x86_64, got '$platform'" ;;
esac
asset=qstar-$tag-$platform.$archive_ext
if test "$platform" = linux-x86_64 && test "$host" != Linux; then
	fail "linux-x86_64 download smoke must be run on a Linux host"
fi
if test "$platform" = macos-arm64 && test "$host" != Darwin; then
	fail "macos-arm64 download smoke must be run on a Darwin host"
fi
if test "$platform" = windows-x86_64 && test "$host_family" != windows; then
	fail "windows-x86_64 download smoke must be run on a Windows/MSYS2 host"
fi

command -v curl >/dev/null 2>&1 || fail "curl is required for GitHub release download smoke"
command -v awk >/dev/null 2>&1 || fail "awk is required for checksum verification"
case "$archive_format" in
	tar.gz)
		command -v tar >/dev/null 2>&1 || fail "tar is required for GitHub release download smoke"
		;;
	zip)
		command -v unzip >/dev/null 2>&1 || fail "unzip is required for windows-x86_64 download smoke"
		;;
	*)
		fail "unsupported archive format '$archive_format'"
		;;
esac

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

list_archive() {
	case "$archive_format" in
	tar.gz)
		tar -tzf "$archive"
		;;
	zip)
		unzip -Z1 "$archive"
		;;
	*)
		fail "unsupported archive format '$archive_format'"
		;;
	esac
}

extract_archive() {
	case "$archive_format" in
	tar.gz)
		tar -xzf "$archive" -C "$install_root"
		;;
	zip)
		unzip -q "$archive" -d "$install_root"
		;;
	*)
		fail "unsupported archive format '$archive_format'"
		;;
	esac
}

run_logged() {
	name=$1
	shift
	"$@" > "$download/$name.out" 2> "$download/$name.err"
}

run_logged_in() {
	name=$1
	cwd=$2
	shift 2
	(
		cd "$cwd"
		"$@"
	) > "$download/$name.out" 2> "$download/$name.err"
}

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

list_archive > "$contents"
for entry in \
	"$binary_rel" \
	share/doc/qstar/wiki/AI_INDEX.md \
	share/doc/qstar/wiki/README.md \
	share/doc/qstar/wiki/reference/qstar-lua.md \
	share/qstar/languages/cuda/cuda.qsm \
	share/qstar/languages/cuda/provider.lua \
	share/qstar/languages/rust/rust.qsm \
	share/qstar/languages/rust/provider.lua \
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
	grep -Fx "$entry" "$contents" >/dev/null || fail "runtime archive is missing '$entry'"
done
if grep -E '\.vsix$' "$contents" >/dev/null; then
	fail "VSCode VSIX must not be included in the public beta runtime archive"
fi

extract_archive
qstar_bin=$install_root/$binary_rel
if test "$platform" = windows-x86_64; then
	test -f "$qstar_bin" || fail "installed qstar.exe binary missing"
else
	test -x "$qstar_bin" || fail "installed qstar binary missing"
fi
actual_version=$("$qstar_bin" --version)
test "$actual_version" = "$expected_version" || \
	fail "installed qstar version '$actual_version' does not match '$expected_version'"

docs_path=$(QSTAR_DOC_DIR="$install_root/share/doc/qstar" "$qstar_bin" docs --path)
path_matches "$docs_path" "$install_root/share/doc/qstar/wiki" || \
	fail "docs --path returned '$docs_path'"
ai_path=$(QSTAR_DOC_DIR="$install_root/share/doc/qstar" "$qstar_bin" docs --ai-index)
path_matches "$ai_path" "$install_root/share/doc/qstar/wiki/AI_INDEX.md" || \
	fail "docs --ai-index returned '$ai_path'"
QSTAR_DOC_DIR="$install_root/share/doc/qstar" \
	"$qstar_bin" docs --show README.md > "$docs_home_report"
grep -F "QStar" "$docs_home_report" >/dev/null || \
	fail "docs --show README.md did not print wiki home"
QSTAR_DOC_DIR="$install_root/share/doc/qstar" \
	"$qstar_bin" docs --show reference/qstar-lua.md > "$docs_lua_report"
grep -F "qstar.project" "$docs_lua_report" >/dev/null || \
	fail "docs --show reference/qstar-lua.md did not print qstar-lua reference"

test -s "$install_root/share/man/man1/qstar.1" || fail "installed qstar(1) manpage missing"
test -s "$install_root/share/man/man5/qstar-lua.5" || fail "installed qstar-lua(5) manpage missing"
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
grep -F ".Dt QSTAR 1" "$install_root/share/man/man1/qstar.1" >/dev/null || \
	fail "installed qstar(1) manpage does not look like qstar"
grep -F ".Dt QSTAR-LUA 5" "$install_root/share/man/man5/qstar-lua.5" >/dev/null || \
	fail "installed qstar-lua(5) manpage does not look like qstar-lua"
if command -v man >/dev/null 2>&1; then
	MANPAGER=cat MANWIDTH=80 man -l "$install_root/share/man/man1/qstar.1" > "$man1_report" 2>/dev/null || true
	MANPAGER=cat MANWIDTH=80 man -l "$install_root/share/man/man5/qstar-lua.5" > "$man5_report" 2>/dev/null || true
fi

if command -v file >/dev/null 2>&1; then
	file "$qstar_bin" > "$file_report"
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
		fail "file(1) is required for $platform download smoke"
		;;
	esac
fi

if test "$platform" = linux-x86_64; then
	command -v ldd >/dev/null 2>&1 || fail "ldd is required for linux-x86_64 download smoke"
	if ! ldd "$qstar_bin" > "$ldd_report" 2>&1; then
		if grep -F "statically linked" "$file_report" >/dev/null; then
			printf 'statically linked binary; ldd is not applicable\n' > "$ldd_report"
		else
			fail "ldd failed for downloaded linux-x86_64 release binary"
		fi
	fi
fi

if test "$host" = Darwin && command -v codesign >/dev/null 2>&1; then
	codesign -dv --verbose=2 "$qstar_bin" > "$download/codesign.txt" 2>&1 || \
		fail "codesign detail verification failed"
	codesign --verify "$qstar_bin" >/dev/null 2>&1 || \
		fail "codesign signature verification failed"
fi

if test "$platform" = windows-x86_64; then
	command -v ninja >/dev/null 2>&1 || fail "ninja is required for windows-x86_64 download smoke"
	cc_tool=${QSTAR_WINDOWS_RELEASE_CC:-gcc}
	cc_path=$(command -v "$cc_tool" 2>/dev/null || true)
	test -n "$cc_path" || fail "could not find '$cc_tool' for windows-x86_64 download smoke"
	fake_bin=$download/fake-bin
	project_root=$download/project-smoke
	mkdir -p "$fake_bin" "$project_root"
	cat > "$fake_bin/cc" <<EOF
#!/bin/sh
exec "$cc_tool" "\$@"
EOF
	chmod +x "$fake_bin/cc"

	hello_name=hello
	run_logged_in init-hello "$project_root" env PATH="$fake_bin:$PATH" \
		"$qstar_bin" init app "$hello_name" --use-language=c
	test -f "$project_root/$hello_name/qstar.lua" || \
		fail "downloaded qstar init app did not create qstar.lua"
	run_logged_in build-hello-stella "$project_root" env PATH="$fake_bin:$PATH" \
		"$qstar_bin" --file "$hello_name/qstar.lua" build //:app
	grep -F "status ok" "$download/build-hello-stella.out" >/dev/null || \
		fail "downloaded qstar Stella build did not finish with status ok"
	run_logged_in build-hello-ninja "$project_root" env PATH="$fake_bin:$PATH" \
		"$qstar_bin" --file "$hello_name/qstar.lua" -G ninja build //:app
	grep -F "backend ninja" "$download/build-hello-ninja.out" >/dev/null || \
		fail "downloaded qstar Ninja build did not use ninja backend"
	grep -F "status ok" "$download/build-hello-ninja.out" >/dev/null || \
		fail "downloaded qstar Ninja build did not finish with status ok"

	zig_name=hello-zig
	run_logged_in init-zig "$project_root" env PATH="$fake_bin:$PATH" \
		"$qstar_bin" init app "$zig_name" --use-language=zig
	grep -F "vendor qstar/languages/zig" "$download/init-zig.out" >/dev/null || \
		fail "downloaded qstar init did not report Zig provider vendoring"
	test -f "$project_root/$zig_name/qstar/languages/zig/zig.qsm" || \
		fail "downloaded qstar init did not vendor Zig manifest"
	test -f "$project_root/$zig_name/qstar/languages/zig/provider.lua" || \
		fail "downloaded qstar init did not vendor Zig implementation"
fi

printf 'qstar-release-download-smoke: tag=%s\n' "$tag"
printf 'qstar-release-download-smoke: platform=%s\n' "$platform"
printf 'qstar-release-download-smoke: asset=%s\n' "$asset"
printf 'qstar-release-download-smoke: sha256=%s\n' "$actual_sha"
printf 'qstar-release-download-smoke: installed_version=%s\n' "$actual_version"
printf 'qstar-release-download-smoke: status=ok\n'
