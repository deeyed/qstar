#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
case "$qstar" in
/*) ;;
*) qstar="$(pwd)/$qstar" ;;
esac

tmp=${TMPDIR:-/tmp}/qstar-typed-dependency-targets.$$
project=$tmp/project
finish() {
	rm -rf "$tmp"
}
trap finish EXIT HUP INT TERM
mkdir -p "$tmp"
cp -R tests/projects/typed-dependency-targets "$project"
chmod +x "$project/tools/copy-tool.sh"

fail() {
	printf 'qstar-typed-dependency-targets: %s\n' "$1" >&2
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

run_capture() {
	name=$1
	shift
	if ! "$@" > "$tmp/$name.out" 2> "$tmp/$name.err"; then
		printf '%s\n' "--- $name.out ---" >&2
		cat "$tmp/$name.out" >&2 || true
		printf '%s\n' "--- $name.err ---" >&2
		cat "$tmp/$name.err" >&2 || true
		fail "$name failed"
	fi
}

cc=${CC:-cc}
ar=${AR:-ar}
mkdir -p "$project/vendor"
"$cc" -c "$project/vendor/vendor.c" -o "$project/vendor/vendor.o"
"$ar" rcs "$project/vendor/libvendor-default.a" "$project/vendor/vendor.o"
"$cc" -c "$project/vendor/private.c" -o "$project/vendor/private.o"
"$ar" rcs "$project/vendor/libprivate.a" "$project/vendor/private.o"
for platform in darwin linux windows; do
	cp "$project/vendor/libvendor-default.a" "$project/vendor/libvendor-$platform.a"
	printf '%s-runtime\n' "$platform" > "$project/vendor/$platform.runtime"
done
printf 'default-runtime\n' > "$project/vendor/default.runtime"

case "$(uname -s)" in
Darwin) selected_runtime=darwin-runtime ;;
Linux) selected_runtime=linux-runtime ;;
*) selected_runtime=windows-runtime ;;
esac

cd "$project"
run_capture check "$qstar" check //...
contains "$tmp/check.out" "status ok"
run_capture query "$qstar" query //:app
contains "$tmp/query.out" "effective_compile_usage.options [-DQSTAR_VENDOR_USAGE=1, -DQSTAR_API_USAGE=1, -DQSTAR_BASE_USAGE=1]"
contains "$tmp/query.out" "effective_link_usage.inputs [contracts/link.contract]"
run_capture explain "$qstar" explain //:app
contains "$tmp/explain.out" "kind imported"
contains "$tmp/explain.out" "role=link"
contains "$tmp/explain.out" "command_argv id=//:app:compile:0"
contains "$tmp/explain.out" "-DQSTAR_VENDOR_USAGE=1"
run_capture targets "$qstar" list-targets --format json
contains "$tmp/targets.out" '"kind":"interface"'
contains "$tmp/targets.out" '"kind":"imported"'
contains "$tmp/targets.out" '"kind":"tool"'
contains "$tmp/targets.out" '"effective_compile_usage"'
contains "$tmp/targets.out" '"imported_artifacts"'

run_capture stella "$qstar" -B build/stella -G stella build //:all --progress off
contains "$tmp/stella.out" "status ok"
"$project/build/stella/out/___app/app" || fail "Stella executable failed"
test -f "$project/build/stella/out/___middle/libmiddle.a" ||
	fail "Stella static library missing"
case "$(uname -s)" in
Darwin) plugin="$project/build/stella/out/___plugin/libplugin.dylib" ;;
*) plugin="$project/build/stella/out/___plugin/libplugin.so" ;;
esac
test -f "$plugin" || fail "Stella shared library missing"
contains "$project/generated/contracts/generated.txt" "generated-usage-input"
contains "$project/generated/tool/output.txt" "built-tool-input"
contains "$project/generated/tool/imported.txt" "built-tool-input"
contains "$project/generated/runtime/selected.txt" "$selected_runtime"
run_capture stella_link_log "$qstar" -B build/stella action-log //:app:link:0
contains "$tmp/stella_link_log.out" "libvendor-"
not_contains "$tmp/stella_link_log.out" "libprivate.a"
run_capture stella_noop "$qstar" -B build/stella -G stella build //:all \
  --progress off --schedule-trace
contains "$tmp/stella_noop.out" "plan_cache status=hit reason=hit"
contains "$tmp/stella_noop.out" "status ok"

command -v ninja >/dev/null 2>&1 || fail "ninja is required"
rm -rf "$project/generated"
run_capture ninja "$qstar" -B build/ninja -G ninja build //:all --progress off
contains "$tmp/ninja.out" "backend ninja"
"$project/build/ninja/out/___app/app" || fail "Ninja executable failed"
test -f "$project/build/ninja/out/___middle/libmiddle.a" ||
	fail "Ninja static library missing"
contains "$project/generated/contracts/generated.txt" "generated-usage-input"
contains "$project/generated/tool/output.txt" "built-tool-input"
contains "$project/generated/tool/imported.txt" "built-tool-input"
contains "$project/generated/runtime/selected.txt" "$selected_runtime"
contains "$project/build/ninja/ninja/build.ninja" "libvendor-"
contains "$project/build/ninja/ninja/build.ninja" "contracts/link.contract"
run_capture ninja_link_log "$qstar" -B build/ninja -G ninja action-log //:app:link:0
contains "$tmp/ninja_link_log.out" "libvendor-"
not_contains "$tmp/ninja_link_log.out" "libprivate.a"
if grep 'command = .*___app/app.*libprivate' \
    "$project/build/ninja/ninja/build.ninja" >/dev/null; then
	fail "interface private dependency artifact leaked into app link command"
fi

if "$qstar" --file negative/interface-unknown.lua check \
    > "$tmp/interface-unknown.out" 2> "$tmp/interface-unknown.err"; then
	fail "interface unknown field unexpectedly succeeded"
fi
contains "$tmp/interface-unknown.err" "unknown field 'compile_usgae'"

if "$qstar" --file negative/imported-no-fallback.lua check \
    > "$tmp/no-fallback.out" 2> "$tmp/no-fallback.err"; then
	fail "imported target without platform fallback unexpectedly succeeded"
fi
contains "$tmp/no-fallback.err" "has no artifacts for platform"

if "$qstar" --file negative/tool-file-kind.lua check \
    > "$tmp/tool-kind.out" 2> "$tmp/tool-kind.err"; then
	fail "non-tool qstar.tool_file unexpectedly succeeded"
fi
contains "$tmp/tool-kind.err" "has no executable tool artifact"

if "$qstar" --file negative/visibility.lua check \
    > "$tmp/visibility.out" 2> "$tmp/visibility.err"; then
	fail "tool_file visibility bypass unexpectedly succeeded"
fi
contains "$tmp/visibility.err" "is not visible to '//negative/blocked:blocked'"

printf 'qstar-typed-dependency-targets: passed\n'
