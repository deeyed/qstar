#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-windows-sharedlib-artifact-parity.$$
corpus=tests/corpus/windows-artifacts
build_dir=build/qstar
artifact_dir=${QSTAR_WINDOWS_SHARED_ARTIFACT_DIR:-}

if test -z "$artifact_dir" && test -n "${QSTAR_WINDOWS_BETA_DIR:-}"; then
	artifact_dir=$QSTAR_WINDOWS_BETA_DIR/windows-sharedlib-detail
fi
if test -z "$artifact_dir" && test -n "${QSTAR_WINDOWS_ALPHA_DIR:-}"; then
	artifact_dir=$QSTAR_WINDOWS_ALPHA_DIR/windows-sharedlib-detail
fi

fail() {
	printf 'qstar-windows-sharedlib-artifact-parity: %s\n' "$1" >&2
	exit 1
}

contains() {
	file=$1
	pattern=$2
	grep -F -q -- "$pattern" "$file" ||
		fail "missing pattern '$pattern' in $file"
}

not_contains() {
	file=$1
	pattern=$2
	if grep -F -q -- "$pattern" "$file"; then
		fail "unexpected pattern '$pattern' in $file"
	fi
}

finish() {
	rc=$?
	set +e
	if test "$rc" -ne 0 && test -n "$artifact_dir"; then
		mkdir -p "$artifact_dir/tmp"
		printf 'status=fail script=windows-sharedlib-artifact-parity rc=%s tmp=%s\n' \
			"$rc" "$tmp" > "$artifact_dir/failure.status"
		if test -d "$tmp"; then
			cp -R "$tmp"/. "$artifact_dir/tmp"/
		fi
		if test -d "$corpus/$build_dir"; then
			mkdir -p "$artifact_dir/windows-artifacts"
			cp -R "$corpus/$build_dir" "$artifact_dir/windows-artifacts/build-qstar"
		fi
		printf 'qstar-windows-sharedlib-artifact-parity: failed rc=%s detail=%s\n' \
			"$rc" "$artifact_dir" >&2
	fi
	rm -rf "$tmp"
	rm -rf "$corpus/build" "$corpus/stage"
	rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps"
	exit "$rc"
}

rm -rf "$tmp"
mkdir -p "$tmp"
rm -rf "$corpus/build" "$corpus/stage"
rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps"
trap finish EXIT HUP INT TERM

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	--qstar-internal-toolchain clang check \
	> "$tmp/check.out" 2> "$tmp/check.err"
contains "$tmp/check.out" "status ok"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	--qstar-internal-toolchain clang explain //:plugin \
	> "$tmp/explain-plugin.out" 2> "$tmp/explain-plugin.err"
contains "$tmp/explain-plugin.out" \
	"artifact id=runtime role=sharedlib path=build/qstar/out/___plugin/plugin.dll install_dir=bin primary=true installable=true"
contains "$tmp/explain-plugin.out" \
	"artifact id=import_lib role=import_lib path=build/qstar/out/___plugin/plugin.lib install_dir=lib primary=false installable=true"
contains "$tmp/explain-plugin.out" "/IMPLIB:build/qstar/out/___plugin/plugin.lib"
not_contains "$tmp/explain-plugin.out" "plan_diagnostic kind=windows-sharedlib-lowering"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	--qstar-internal-toolchain clang dry-run //:plugin_user \
	> "$tmp/dry-plugin-user.out" 2> "$tmp/dry-plugin-user.err"
contains "$tmp/dry-plugin-user.out" "output=build/qstar/out/___plugin/plugin.dll"
contains "$tmp/dry-plugin-user.out" \
	"artifact id=import_lib role=import_lib path=build/qstar/out/___plugin/plugin.lib"
contains "$tmp/dry-plugin-user.out" "/IMPLIB:build/qstar/out/___plugin/plugin.lib"
not_contains "$tmp/dry-plugin-user.out" "plan_diagnostic kind=windows-sharedlib-lowering"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	--qstar-internal-toolchain clang list-targets --format json \
	> "$tmp/list-targets.json" 2> "$tmp/list-targets.err"
contains "$tmp/list-targets.json" \
	"\"id\":\"runtime\",\"role\":\"sharedlib\",\"path\":\"build/qstar/out/___plugin/plugin.dll\",\"install_dir\":\"bin\",\"primary\":true,\"installable\":true"
contains "$tmp/list-targets.json" \
	"\"id\":\"import_lib\",\"role\":\"import_lib\",\"path\":\"build/qstar/out/___plugin/plugin.lib\",\"install_dir\":\"lib\",\"primary\":false,\"installable\":true"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	--qstar-internal-toolchain clang --progress off build //:plugin_user \
	> "$tmp/build-plugin-user.out" 2> "$tmp/build-plugin-user.err"
contains "$tmp/build-plugin-user.out" "status ok"
test -x "$corpus/$build_dir/out/___plugin/plugin.dll" ||
	fail "Stella Windows sharedlib runtime plugin.dll missing"
test -f "$corpus/$build_dir/out/___plugin/plugin.lib" ||
	fail "Stella Windows sharedlib import plugin.lib missing"
test -x "$corpus/$build_dir/out/___plugin_user/plugin_user.exe" ||
	fail "Stella Windows sharedlib consumer plugin_user.exe missing"
contains "$corpus/$build_dir/out/___plugin/plugin.lib" "fake import library"
contains "$corpus/$build_dir/out/___plugin/plugin.lib" \
	"runtime=build/qstar/out/___plugin/plugin.dll"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	--qstar-internal-toolchain clang action-log //:plugin:link-shared:0 \
	> "$tmp/action-log-plugin.out" 2> "$tmp/action-log-plugin.err"
contains "$tmp/action-log-plugin.out" "output_count=2"
contains "$tmp/action-log-plugin.out" "output[0]=build/qstar/out/___plugin/plugin.dll"
contains "$tmp/action-log-plugin.out" "output[1]=build/qstar/out/___plugin/plugin.lib"
contains "$tmp/action-log-plugin.out" "/IMPLIB:build/qstar/out/___plugin/plugin.lib"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	--qstar-internal-toolchain clang action-log //:plugin_user:link:0 \
	> "$tmp/action-log-plugin-user.out" 2> "$tmp/action-log-plugin-user.err"
contains "$tmp/action-log-plugin-user.out" "build/qstar/out/___plugin/plugin.lib"
not_contains "$tmp/action-log-plugin-user.out" "build/qstar/out/___plugin/plugin.dll"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	--qstar-internal-toolchain clang stage //:plugin_layout \
	> "$tmp/stage-plugin.out" 2> "$tmp/stage-plugin.err"
contains "$tmp/stage-plugin.out" "status ok"
test -f "$corpus/$build_dir/stage/windows-plugin/bin/plugin.dll" ||
	fail "Stella Windows sharedlib staged runtime plugin.dll missing"
test -f "$corpus/$build_dir/stage/windows-plugin/lib/plugin.lib" ||
	fail "Stella Windows sharedlib staged import plugin.lib missing"
stage_manifest="$corpus/$build_dir/stage/___plugin_layout/manifest.json"
contains "$stage_manifest" "\"artifact\":\"runtime\""
contains "$stage_manifest" "\"artifact\":\"import_lib\""
not_contains "$stage_manifest" "\\"

if command -v ninja >/dev/null 2>&1; then
	rm -rf "$corpus/$build_dir" "$corpus/.ninja_log" "$corpus/.ninja_deps"
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
		--qstar-internal-toolchain clang -G ninja --progress off build //:plugin_user \
		> "$tmp/ninja-build-plugin-user.out" \
		2> "$tmp/ninja-build-plugin-user.err"
	contains "$tmp/ninja-build-plugin-user.out" "backend ninja"
	contains "$tmp/ninja-build-plugin-user.out" "status ok"
	test -x "$corpus/$build_dir/out/___plugin/plugin.dll" ||
		fail "Ninja Windows sharedlib runtime plugin.dll missing"
	test -f "$corpus/$build_dir/out/___plugin/plugin.lib" ||
		fail "Ninja Windows sharedlib import plugin.lib missing"
	test -x "$corpus/$build_dir/out/___plugin_user/plugin_user.exe" ||
		fail "Ninja Windows sharedlib consumer plugin_user.exe missing"
	contains "$corpus/$build_dir/ninja/build.ninja" \
		"build/qstar/out/___plugin/plugin.dll build/qstar/out/___plugin/plugin.lib: qstar_link"
	contains "$corpus/$build_dir/ninja/build.ninja" \
		"build/qstar/out/___plugin_user/plugin_user.exe: qstar_link build/qstar/out/___plugin_user/obj0.o build/qstar/out/___plugin/plugin.lib"
	test ! -f "$corpus/.ninja_log" ||
		fail "Windows sharedlib corpus root .ninja_log pollution"
	test ! -f "$corpus/.ninja_deps" ||
		fail "Windows sharedlib corpus root .ninja_deps pollution"

	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
		--qstar-internal-toolchain clang -G ninja action-log //:plugin:link-shared:0 \
		> "$tmp/ninja-action-log-plugin.out" \
		2> "$tmp/ninja-action-log-plugin.err"
	contains "$tmp/ninja-action-log-plugin.out" "backend=ninja"
	contains "$tmp/ninja-action-log-plugin.out" "output_count=2"
	contains "$tmp/ninja-action-log-plugin.out" "/IMPLIB:build/qstar/out/___plugin/plugin.lib"

	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
		--qstar-internal-toolchain clang -G ninja action-log //:plugin_user:link:0 \
		> "$tmp/ninja-action-log-plugin-user.out" \
		2> "$tmp/ninja-action-log-plugin-user.err"
	contains "$tmp/ninja-action-log-plugin-user.out" "backend=ninja"
	contains "$tmp/ninja-action-log-plugin-user.out" "build/qstar/out/___plugin/plugin.lib"
	not_contains "$tmp/ninja-action-log-plugin-user.out" "build/qstar/out/___plugin/plugin.dll"

	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
		--qstar-internal-toolchain clang -G ninja stage //:plugin_layout \
		> "$tmp/ninja-stage-plugin.out" 2> "$tmp/ninja-stage-plugin.err"
	contains "$tmp/ninja-stage-plugin.out" "backend ninja"
	test -f "$corpus/$build_dir/stage/windows-plugin/bin/plugin.dll" ||
		fail "Ninja Windows sharedlib staged runtime plugin.dll missing"
	test -f "$corpus/$build_dir/stage/windows-plugin/lib/plugin.lib" ||
		fail "Ninja Windows sharedlib staged import plugin.lib missing"
	printf 'qstar-windows-sharedlib-artifact-parity: ninja=passed\n'
else
	printf 'qstar-windows-sharedlib-artifact-parity: ninja=skipped reason=ninja-not-found\n'
fi

printf 'qstar-windows-sharedlib-artifact-parity: passed\n'
