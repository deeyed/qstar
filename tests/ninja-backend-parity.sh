#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-ninja-backend-parity.$$
c_app=tests/corpus/c-app
generated=tests/corpus/generated

fail() {
	echo "qstar-ninja-backend-parity: $*" >&2
	exit 1
}

contains() {
	file=$1
	pat=$2
	grep -F -q -- "$pat" "$file" || fail "missing pattern '$pat' in $file"
}

clean_corpus_outputs() {
	rm -rf "$c_app/build" "$c_app/stage" "$generated/build" "$generated/stage"
	rm -f "$c_app/.ninja_log" "$c_app/.ninja_deps"
	rm -f "$generated/.ninja_log" "$generated/.ninja_deps"
}

rm -rf "$tmp"
mkdir -p "$tmp"
clean_corpus_outputs
trap 'rm -rf "$tmp"; clean_corpus_outputs' EXIT HUP INT TERM

"$qstar" --file "$c_app/qstar.lua" emit-ninja //:all > "$tmp/c-app-emit.out" 2> "$tmp/c-app-emit.err"
contains "$tmp/c-app-emit.out" "ninja_file build/qstar/ninja/build.ninja"
contains "$c_app/build/qstar/ninja/build.ninja" "qstar_action_id = //:core:archive:0"
contains "$c_app/build/qstar/ninja/build.ninja" "qstar_action_id = //:app:link:0"
contains "$c_app/build/qstar/ninja/build.ninja" "qstar_action_id = //:unit:link:0"
contains "$c_app/build/qstar/ninja/build.ninja" "qstar_action_id = //:all:group:0"
contains "$c_app/build/qstar/ninja/build.ninja" "description = Building C object build/qstar/out/___core/obj0.o"
contains "$c_app/build/qstar/ninja/build.ninja" "description = Linking C static library build/qstar/out/___core/libcore.a"
contains "$c_app/build/qstar/ninja/build.ninja" "description = Linking C executable build/qstar/out/___app/app"
contains "$c_app/build/qstar/compile_commands.json" "src/core.c"
contains "$c_app/build/qstar/compile_commands.json" "src/main.c"
"$qstar" --file "$c_app/qstar.lua" action-log //:app:link:0 > "$tmp/c-app-link-log.out" 2> "$tmp/c-app-link-log.err"
contains "$tmp/c-app-link-log.out" "qstar action-log v1"
contains "$tmp/c-app-link-log.out" "backend=ninja"
contains "$tmp/c-app-link-log.out" "description='Linking C executable build/qstar/out/___app/app'"
"$qstar" --file "$c_app/qstar.lua" replay //:app:link:0 > "$tmp/c-app-link-replay.out" 2> "$tmp/c-app-link-replay.err"
contains "$tmp/c-app-link-replay.out" "qstar replay v1"
contains "$tmp/c-app-link-replay.out" "description='Linking C executable build/qstar/out/___app/app'"
contains "$tmp/c-app-link-replay.out" "build/qstar/out/___app/app"

"$qstar" --file "$generated/qstar.lua" emit-ninja //:all > "$tmp/generated-emit.out" 2> "$tmp/generated-emit.err"
contains "$generated/build/qstar/ninja/build.ninja" "rule qstar_generate"
contains "$generated/build/qstar/ninja/build.ninja" "qstar_action_id = //:cfg:generate:0"
contains "$generated/build/qstar/ninja/build.ninja" "qstar_action_id = //:make_value:generate:0"
contains "$generated/build/qstar/ninja/build.ninja" "qstar_action_id = //:app:link:0"
contains "$generated/build/qstar/ninja/build.ninja" "qstar_action_id = //:smoke:run:0"
contains "$generated/build/qstar/ninja/build.ninja" "description = Configuring generated config.h"
contains "$generated/build/qstar/ninja/build.ninja" "description = Generating generated value.c"
contains "$generated/build/qstar/ninja/build.ninja" "description = Running generated smoke"
"$qstar" --file "$generated/qstar.lua" action-log //:make_value:generate:0 > "$tmp/generated-log.out" 2> "$tmp/generated-log.err"
contains "$tmp/generated-log.out" "qstar action-log v1"
contains "$tmp/generated-log.out" "backend=ninja"
contains "$tmp/generated-log.out" "description='Generating generated value.c'"
contains "$tmp/generated-log.out" "tools/gen-value.sh"
"$qstar" --file "$generated/qstar.lua" replay //:make_value:generate:0 > "$tmp/generated-replay.out" 2> "$tmp/generated-replay.err"
contains "$tmp/generated-replay.out" "qstar replay v1"
contains "$tmp/generated-replay.out" "description='Generating generated value.c'"
contains "$tmp/generated-replay.out" "build/qstar/generated/value.c"

mkdir -p "$tmp/shared/src"
cat > "$tmp/shared/qstar.lua" <<'QSTAR'
qstar.sharedlib "plugin" {
  sources = {"src/plugin.c"},
}
QSTAR
cat > "$tmp/shared/src/plugin.c" <<'SRC'
int plugin_value(void) { return 9; }
SRC
if "$qstar" --file "$tmp/shared/qstar.lua" -G ninja build //:plugin > "$tmp/shared.out" 2> "$tmp/shared.err"; then
	fail "sharedlib Ninja lowering unexpectedly succeeded"
fi
contains "$tmp/shared.err" "sharedlib target '//:plugin' is not lowered by the ninja backend yet"
contains "$tmp/shared.err" "plan/check-only"

if command -v ninja >/dev/null 2>&1; then
	"$qstar" --file "$c_app/qstar.lua" -G ninja build //:app --progress off > "$tmp/c-app-build.out" 2> "$tmp/c-app-build.err"
	contains "$tmp/c-app-build.out" "backend ninja"
	contains "$tmp/c-app-build.out" "status ok"
	test -f "$c_app/build/qstar/out/___app/app" || fail "c-app ninja executable missing"
	"$qstar" --file "$c_app/qstar.lua" -G ninja test //:unit > "$tmp/c-app-test.out" 2> "$tmp/c-app-test.err"
	contains "$tmp/c-app-test.out" "backend ninja"
	contains "$tmp/c-app-test.out" "test_result label=//:unit status=pass"
	"$qstar" --file "$c_app/qstar.lua" -G ninja install //:core --prefix "$tmp/c-app-prefix" > "$tmp/c-app-install.out" 2> "$tmp/c-app-install.err"
	contains "$tmp/c-app-install.out" "backend ninja"
	test -f "$tmp/c-app-prefix/lib/libcore.a" || fail "c-app ninja install lib missing"
	test -f "$tmp/c-app-prefix/include/corpus.h" || fail "c-app ninja install header missing"
	test ! -f "$c_app/.ninja_log" || fail "c-app ninja wrote package root .ninja_log"
	test ! -f "$c_app/.ninja_deps" || fail "c-app ninja wrote package root .ninja_deps"

	"$qstar" --file "$generated/qstar.lua" -G ninja build //:all --progress off > "$tmp/generated-build.out" 2> "$tmp/generated-build.err"
	contains "$tmp/generated-build.out" "backend ninja"
	contains "$tmp/generated-build.out" "Configuring generated config.h"
	contains "$tmp/generated-build.out" "Generating generated value.c"
	contains "$tmp/generated-build.out" "Running generated smoke"
	contains "$tmp/generated-build.out" "run_marker label=//:smoke status=matched"
	contains "$tmp/generated-build.out" "status ok"
	test -f "$generated/build/qstar/out/___app/app" || fail "generated ninja app missing"
	test -f "$generated/build/qstar/generated/config.h" || fail "generated ninja config missing"
	test -f "$generated/build/qstar/generated/value.c" || fail "generated ninja source missing"
	"$qstar" --file "$generated/qstar.lua" -G ninja stage //:bundle > "$tmp/generated-stage.out" 2> "$tmp/generated-stage.err"
	contains "$tmp/generated-stage.out" "backend ninja"
	contains "$tmp/generated-stage.out" "stage_file src=build/qstar/out/___app/app"
	test -f "$generated/stage/bundle/bin/app" || fail "generated ninja stage app missing"
	test -f "$generated/stage/bundle/share/value.c" || fail "generated ninja stage source missing"
	"$qstar" --file "$generated/qstar.lua" -G ninja install //:app --prefix "$tmp/generated-prefix" > "$tmp/generated-install.out" 2> "$tmp/generated-install.err"
	contains "$tmp/generated-install.out" "backend ninja"
	test -f "$tmp/generated-prefix/bin/app" || fail "generated ninja install app missing"
	test ! -f "$generated/.ninja_log" || fail "generated ninja wrote package root .ninja_log"
	test ! -f "$generated/.ninja_deps" || fail "generated ninja wrote package root .ninja_deps"
else
	printf 'qstar-ninja-backend-parity: ninja runtime checks skipped reason=ninja-not-found\n'
fi

printf 'qstar-ninja-backend-parity: passed\n'
