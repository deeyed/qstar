#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=$(sed -n 's/^#define QSTAR_VERSION "\(.*\)"/\1/p' "$repo_dir/include/qstar/qstar.h")
tmp_root=${TMPDIR:-/tmp}/qstar-windows-release-asset.$$
cleanup_tmp=0
if test -n "${QSTAR_WINDOWS_RELEASE_DIST:-}"; then
	dist=$QSTAR_WINDOWS_RELEASE_DIST
else
	dist=$tmp_root/release-package
	cleanup_tmp=1
fi
if test -n "${QSTAR_WINDOWS_RELEASE_SMOKE_DIR:-}"; then
	smoke_dir=$QSTAR_WINDOWS_RELEASE_SMOKE_DIR
else
	smoke_dir=$tmp_root/release-asset-smoke
	cleanup_tmp=1
fi
detail_dir=${QSTAR_WINDOWS_RELEASE_ARTIFACT_DIR:-}
cc_tool=${QSTAR_WINDOWS_RELEASE_CC:-gcc}
host=$(uname -s 2>/dev/null || printf unknown)
native_windows=0

case "$dist" in
/*) ;;
*) dist=$repo_dir/$dist ;;
esac
case "$smoke_dir" in
/*) ;;
*) smoke_dir=$repo_dir/$smoke_dir ;;
esac
if test -n "$detail_dir"; then
	case "$detail_dir" in
	/*) ;;
	*) detail_dir=$repo_dir/$detail_dir ;;
	esac
fi

case "$host" in
MINGW*|MSYS*|CYGWIN*)
	native_windows=1
	;;
esac

fail() {
	printf 'qstar-windows-release-asset: %s\n' "$1" >&2
	exit 1
}

contains() {
	file=$1
	pattern=$2
	grep -F -q -- "$pattern" "$file" ||
		fail "missing pattern '$pattern' in $file"
}

contains_path() {
	file=$1
	expected=$2
	if grep -F -q -- "$expected" "$file"; then
		return 0
	fi
	if command -v cygpath >/dev/null 2>&1; then
		expected_mixed=$(cygpath -m "$expected" 2>/dev/null || true)
		if test -n "$expected_mixed" && grep -F -q -- "$expected_mixed" "$file"; then
			return 0
		fi
	fi
	fail "missing path '$expected' in $file"
}

finish() {
	rc=$?
	set +e
	if test "$rc" -ne 0 && test -n "$detail_dir"; then
		mkdir -p "$detail_dir"
		printf 'status=fail script=windows-release-asset rc=%s package=%s smoke=%s\n' \
			"$rc" "$dist" "$smoke_dir" > "$detail_dir/failure.status"
		if test -d "$dist"; then
			rm -rf "$detail_dir/release-package"
			cp -R "$dist" "$detail_dir/release-package"
		fi
		if test -d "$smoke_dir"; then
			rm -rf "$detail_dir/release-asset-smoke"
			cp -R "$smoke_dir" "$detail_dir/release-asset-smoke"
		fi
	fi
	if test "$rc" -eq 0 && test "$cleanup_tmp" -ne 0; then
		rm -rf "$tmp_root"
	fi
	exit "$rc"
}

run_logged() {
	name=$1
	shift
	"$@" > "$smoke_dir/logs/$name.out" 2> "$smoke_dir/logs/$name.err"
}

run_logged_in() {
	name=$1
	cwd=$2
	shift 2
	(
		cd "$cwd"
		"$@"
	) > "$smoke_dir/logs/$name.out" 2> "$smoke_dir/logs/$name.err"
}

find_one() {
	pattern=$1
	result=$(find "$dist" -maxdepth 1 -name "$pattern" -print -quit)
	test -n "$result" || fail "missing '$pattern' in $dist"
	printf '%s\n' "$result"
}

rewrite_host_cc() {
	file=$1
	tmp=$file.tmp
	sed "s/qstar.cli {\"cc\"}/qstar.cli {\"$cc_tool\"}/g" "$file" > "$tmp"
	mv "$tmp" "$file"
}

test -n "$version" || fail "could not read QSTAR_VERSION"

rm -rf "$smoke_dir"
mkdir -p "$smoke_dir/logs"
trap finish EXIT HUP INT TERM

if test "$native_windows" -eq 0; then
	rm -rf "$dist"
	mkdir -p "$dist"
	QSTAR_RELEASE_PLATFORM=windows-x86_64 \
	QSTAR_RELEASE_DRY_RUN=1 \
	QSTAR_RELEASE_DIST="$dist" \
		sh "$repo_dir/tools/package-public-beta.sh" \
		> "$smoke_dir/logs/package-dry-run.out" \
		2> "$smoke_dir/logs/package-dry-run.err"
	plan=$dist/qstar-v$version-windows-x86_64.package-plan.txt
	expected=$dist/qstar-v$version-windows-x86_64.expected-contents.txt
	test -f "$plan" || fail "missing Windows release package dry-run plan"
	test -f "$expected" || fail "missing Windows release expected contents"
	contains "$plan" "asset=qstar-v$version-windows-x86_64.zip"
	contains "$plan" "binary=bin/qstar.exe"
	contains "$expected" "bin/qstar.exe"
	printf 'qstar-windows-release-asset: host=%s mode=contract-only\n' "$host"
	printf 'qstar-windows-release-asset: passed\n'
	exit 0
fi

rm -rf "$dist"
mkdir -p "$dist"

QSTAR_RELEASE_PLATFORM=windows-x86_64 \
QSTAR_RELEASE_DIST="$dist" \
	sh "$repo_dir/tools/package-public-beta.sh" \
	> "$smoke_dir/logs/package.out" \
	2> "$smoke_dir/logs/package.err"

asset=$(find_one "qstar-v$version-windows-x86_64.zip")
sha_file=$dist/SHA256SUMS
contents_file=$dist/qstar-v$version-windows-x86_64.contents.txt
plan=$dist/qstar-v$version-windows-x86_64.package-plan.txt
test -f "$sha_file" || fail "missing SHA256SUMS"
test -f "$contents_file" || fail "missing Windows package contents report"
test -f "$plan" || fail "missing Windows package plan"
contains "$contents_file" "bin/qstar.exe"
contains "$contents_file" "share/doc/qstar/wiki/AI_INDEX.md"
contains "$contents_file" "share/qstar/languages/zig/zig.qsm"
contains "$sha_file" "$(basename "$asset")"

extract=$smoke_dir/extract
rm -rf "$extract"
mkdir -p "$extract"
unzip -q "$asset" -d "$extract"
qstar_bin=$extract/bin/qstar.exe
test -f "$qstar_bin" || fail "extracted qstar.exe missing"

run_logged version "$qstar_bin" --version
contains "$smoke_dir/logs/version.out" "qstar $version"
run_logged docs-path env QSTAR_DOC_DIR="$extract/share/doc/qstar" \
	"$qstar_bin" docs --path
contains_path "$smoke_dir/logs/docs-path.out" "$extract/share/doc/qstar/wiki"
run_logged docs-ai-index env QSTAR_DOC_DIR="$extract/share/doc/qstar" \
	"$qstar_bin" docs --ai-index
contains_path "$smoke_dir/logs/docs-ai-index.out" "$extract/share/doc/qstar/wiki/AI_INDEX.md"
run_logged docs-show env QSTAR_DOC_DIR="$extract/share/doc/qstar" \
	"$qstar_bin" docs --show reference/qstar-lua.md
contains "$smoke_dir/logs/docs-show.out" "qstar.project"

test -f "$extract/share/qstar/languages/zig/zig.qsm" ||
	fail "extracted Zig provider manifest missing"
test -f "$extract/share/qstar/languages/zig/provider.lua" ||
	fail "extracted Zig provider implementation missing"
test -f "$extract/share/qstar/languages/rust/rust.qsm" ||
	fail "extracted Rust provider manifest missing"
test -f "$extract/share/qstar/languages/cuda/cuda.qsm" ||
	fail "extracted CUDA provider manifest missing"

fake_bin=$smoke_dir/fake-bin
mkdir -p "$fake_bin"
cc_path=$(command -v "$cc_tool" 2>/dev/null || true)
test -n "$cc_path" || fail "could not find '$cc_tool' for fake cc shim"
cat > "$fake_bin/cc" <<EOF
#!/bin/sh
exec "$cc_path" "\$@"
EOF
chmod +x "$fake_bin/cc"

hello_name=hello
hello=$smoke_dir/$hello_name
run_logged_in init-hello "$smoke_dir" env PATH="$fake_bin:$PATH" \
	"$qstar_bin" init app "$hello_name" --use-language=c
test -f "$hello/qstar.lua" || fail "qstar init app did not create qstar.lua"
rewrite_host_cc "$hello/qstar.lua"
run_logged_in build-hello-stella "$smoke_dir" env PATH="$fake_bin:$PATH" \
	"$qstar_bin" --file "$hello_name/qstar.lua" build //:app
contains "$smoke_dir/logs/build-hello-stella.out" "status ok"
hello_exe=$(find "$hello/build/qstar/out" -name 'app.exe' -o -name 'app' | head -n 1)
test -n "$hello_exe" || fail "qstar init app build did not produce an app artifact"
run_logged run-hello "$hello_exe"

zig_name=hello-zig
zig_project=$smoke_dir/$zig_name
run_logged_in init-zig "$smoke_dir" env PATH="$fake_bin:$PATH" \
	"$qstar_bin" init app "$zig_name" --use-language=zig
contains "$smoke_dir/logs/init-zig.out" "vendor qstar/languages/zig"
test -f "$zig_project/qstar/languages/zig/zig.qsm" ||
	fail "extracted qstar init did not vendor Zig manifest"
test -f "$zig_project/qstar/languages/zig/provider.lua" ||
	fail "extracted qstar init did not vendor Zig implementation"

corpus_name=windows-execution-corpus
corpus=$smoke_dir/$corpus_name
rm -rf "$corpus"
cp -R "$repo_dir/tests/corpus/windows-execution" "$corpus"
run_logged_in corpus-stella "$smoke_dir" env PATH="$fake_bin:$PATH" \
	"$qstar_bin" --file "$corpus_name/qstar.lua" build //:hello_smoke
contains "$smoke_dir/logs/corpus-stella.out" "status ok"

if command -v ninja >/dev/null 2>&1; then
	rm -rf "$corpus/build" "$corpus/stage" "$corpus/.ninja_log" "$corpus/.ninja_deps"
	run_logged_in corpus-ninja "$smoke_dir" env PATH="$fake_bin:$PATH" \
		"$qstar_bin" --file "$corpus_name/qstar.lua" -G ninja build //:hello_smoke
	contains "$smoke_dir/logs/corpus-ninja.out" "backend ninja"
	contains "$smoke_dir/logs/corpus-ninja.out" "status ok"
else
	printf 'qstar-windows-release-asset: ninja=skipped reason=ninja-not-found\n' \
		> "$smoke_dir/logs/corpus-ninja.out"
fi

printf 'status=ok script=windows-release-asset asset=%s smoke=%s\n' \
	"$asset" "$smoke_dir" > "$smoke_dir/release-asset.status"
printf 'qstar-windows-release-asset: asset=%s\n' "$asset"
printf 'qstar-windows-release-asset: smoke=%s\n' "$smoke_dir"
printf 'qstar-windows-release-asset: passed\n'
