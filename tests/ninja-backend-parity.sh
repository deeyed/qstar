#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-ninja-backend-parity.$$
c_app=tests/corpus/c-app
generated=tests/corpus/generated
object_bridge=tests/projects/object-artifact-bridge
host=$(uname -s 2>/dev/null || printf unknown)
host_windows=0
case "$host" in
MINGW*|MSYS*|CYGWIN*)
	host_windows=1
	;;
esac

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
	rm -rf "$object_bridge/build" "$object_bridge/stage"
	rm -f "$c_app/.ninja_log" "$c_app/.ninja_deps"
	rm -f "$generated/.ninja_log" "$generated/.ninja_deps"
	rm -f "$object_bridge/.ninja_log" "$object_bridge/.ninja_deps"
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

case "$host" in
	Darwin) object_shared_artifact="build/qstar/out/___objc_plugin/libobjc_plugin.dylib" ;;
	*) object_shared_artifact="build/qstar/out/___objc_plugin/libobjc_plugin.so" ;;
esac
if [ "$host_windows" -eq 1 ]; then
	"$qstar" --file "$object_bridge/qstar.lua" dry-run //:app \
		> "$tmp/object-bridge-dry.out" 2> "$tmp/object-bridge-dry.err"
	contains "$tmp/object-bridge-dry.out" "generated_artifact output=build/qstar/generated/objc/AppDelegate.o group=objects format=object"
	contains "$tmp/object-bridge-dry.out" "dry_run_step id=//:app:link-input:1"
	"$qstar" --file "$object_bridge/qstar.lua" dry-run //:objc_static \
		> "$tmp/object-bridge-static-dry.out" 2> "$tmp/object-bridge-static-dry.err"
	contains "$tmp/object-bridge-static-dry.out" "dry_run_step id=//:objc_static:link-input:0"
	"$qstar" --file "$object_bridge/qstar.lua" emit-ninja //:app \
		> "$tmp/object-bridge-emit.out" 2> "$tmp/object-bridge-emit.err"
	contains "$tmp/object-bridge-emit.out" "ninja_file build/qstar/ninja/build.ninja"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "qstar_action_id = //:objc_object:generate:0"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "description = Building Objective-C object AppDelegate.o"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "build/qstar/out/___app/app: qstar_link build/qstar/out/___app/obj0.o build/qstar/generated/objc/AppDelegate.o"
	"$qstar" --file "$object_bridge/qstar.lua" emit-ninja //:objc_static \
		> "$tmp/object-bridge-static-emit.out" 2> "$tmp/object-bridge-static-emit.err"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "build/qstar/out/___objc_static/libobjc_static.a: qstar_archive build/qstar/generated/objc/AppDelegate.o"
else
	"$qstar" --file "$object_bridge/qstar.lua" dry-run //:all > "$tmp/object-bridge-dry.out" 2> "$tmp/object-bridge-dry.err"
	contains "$tmp/object-bridge-dry.out" "generated_artifact output=build/qstar/generated/objc/AppDelegate.o group=objects format=object"
	contains "$tmp/object-bridge-dry.out" "dry_run_step id=//:app:link-input:1"
	contains "$tmp/object-bridge-dry.out" "dry_run_step id=//:objc_static:link-input:0"
	contains "$tmp/object-bridge-dry.out" "dry_run_step id=//:objc_plugin:link-input:1"
	"$qstar" --file "$object_bridge/qstar.lua" emit-ninja //:all > "$tmp/object-bridge-emit.out" 2> "$tmp/object-bridge-emit.err"
	contains "$tmp/object-bridge-emit.out" "ninja_file build/qstar/ninja/build.ninja"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "qstar_action_id = //:objc_object:generate:0"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "description = Building Objective-C object AppDelegate.o"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "build/qstar/out/___app/app: qstar_link build/qstar/out/___app/obj0.o build/qstar/generated/objc/AppDelegate.o"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "build/qstar/out/___objc_static/libobjc_static.a: qstar_archive build/qstar/generated/objc/AppDelegate.o"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "$object_shared_artifact"
fi
"$qstar" --file "$object_bridge/qstar.lua" action-log //:objc_object:generate:0 > "$tmp/object-bridge-log.out" 2> "$tmp/object-bridge-log.err"
contains "$tmp/object-bridge-log.out" "qstar action-log v1"
contains "$tmp/object-bridge-log.out" "backend=ninja"
contains "$tmp/object-bridge-log.out" "description='Building Objective-C object AppDelegate.o'"
"$qstar" --file "$object_bridge/qstar.lua" replay //:objc_object:generate:0 > "$tmp/object-bridge-replay.out" 2> "$tmp/object-bridge-replay.err"
contains "$tmp/object-bridge-replay.out" "qstar replay v1"
contains "$tmp/object-bridge-replay.out" "tools/fake-objc-compile.sh src/AppDelegate.m build/qstar/generated/objc/AppDelegate.o"

mkdir -p "$tmp/shared/src"
cat > "$tmp/shared/qstar.lua" <<'QSTAR'
qstar.sharedlib "plugin" {
  sources = {"src/plugin.c"},
}

qstar.executable "plugin_app" {
  sources = {"src/main.c"},
  deps = {"//:plugin"},
}

qstar.test "plugin_test" {
  sources = {"src/main.c"},
  deps = {"//:plugin"},
}

qstar.stage "shared_bundle" {
  root = "stage/shared",
  files = {
    qstar.stage_file(qstar.target_file("//:plugin"), "lib/plugin.shared"),
  },
}

QSTAR
cat > "$tmp/shared/src/plugin.c" <<'SRC'
int plugin_value(void) { return 9; }
SRC
cat > "$tmp/shared/src/main.c" <<'SRC'
int plugin_value(void);
int main(void) { return plugin_value() - 9; }
SRC

mkdir -p "$tmp/link-input/src" "$tmp/link-input/link"
cat > "$tmp/link-input/qstar.lua" <<'QSTAR'
local artifact_name = nil
if qstar.host.os == "windows" then
  artifact_name = "app.exe"
end

qstar.executable "app" {
  sources = {"src/main.c"},
  link_inputs = {"link/input.txt"},
  artifact_name = artifact_name,
}
QSTAR
link_input_artifact="build/qstar/out/___app/app"
if [ "$host_windows" -eq 1 ]; then
	link_input_artifact="build/qstar/out/___app/app.exe"
fi
cat > "$tmp/link-input/src/main.c" <<'SRC'
int main(void) { return 0; }
SRC
cat > "$tmp/link-input/link/input.txt" <<'TXT'
first
TXT
"$qstar" --file "$tmp/link-input/qstar.lua" build //:app --progress off > "$tmp/link-input-stella-build.out" 2> "$tmp/link-input-stella-build.err"
contains "$tmp/link-input-stella-build.out" "status ok"
cat > "$tmp/link-input/link/input.txt" <<'TXT'
second
TXT
"$qstar" --file "$tmp/link-input/qstar.lua" build //:app --explain-cache --verbose > "$tmp/link-input-stella-rebuild.out" 2> "$tmp/link-input-stella-rebuild.err"
contains "$tmp/link-input-stella-rebuild.out" "cache_miss id=//:app:link:0"
contains "$tmp/link-input-stella-rebuild.out" "reason=input-changed"
"$qstar" --file "$tmp/link-input/qstar.lua" emit-ninja //:app > "$tmp/link-input-emit.out" 2> "$tmp/link-input-emit.err"
contains "$tmp/link-input/build/qstar/ninja/build.ninja" "$link_input_artifact: qstar_link build/qstar/out/___app/obj0.o | link/input.txt"

case "$(uname -s)" in
	Darwin)
		shared_artifact="build/qstar/out/___plugin/libplugin.dylib"
		shared_flag="-dynamiclib"
		shared_name_flag="@rpath/libplugin.dylib"
		shared_ninja_rpath_flag="-Wl,-rpath,@loader_path/../___plugin"
		;;
	Linux)
		shared_artifact="build/qstar/out/___plugin/libplugin.so"
		shared_flag="-shared"
		shared_name_flag="-Wl,-soname,libplugin.so"
		shared_ninja_rpath_flag='-Wl,-rpath,$$ORIGIN/../___plugin'
		;;
	*)
		shared_artifact="build/qstar/out/___plugin/libplugin.so"
		shared_flag="-shared"
		shared_name_flag="-Wl,-soname,libplugin.so"
		shared_ninja_rpath_flag='-Wl,-rpath,$$ORIGIN/../___plugin'
		;;
esac
if [ "$host_windows" -eq 0 ]; then
	"$qstar" --file "$tmp/shared/qstar.lua" emit-ninja //:plugin_app > "$tmp/shared-emit.out" 2> "$tmp/shared-emit.err"
	contains "$tmp/shared/build/qstar/ninja/build.ninja" "qstar_action_id = //:plugin:link-shared:0"
	contains "$tmp/shared/build/qstar/ninja/build.ninja" "qstar_action_id = //:plugin_app:link:0"
	contains "$tmp/shared/build/qstar/ninja/build.ninja" "description = Linking C shared library $shared_artifact"
	contains "$tmp/shared/build/qstar/ninja/build.ninja" "$shared_flag"
	contains "$tmp/shared/build/qstar/ninja/build.ninja" "$shared_name_flag"
	contains "$tmp/shared/build/qstar/ninja/build.ninja" "$shared_ninja_rpath_flag"
fi
if "$qstar" --file "$tmp/shared/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang emit-ninja //:plugin > "$tmp/shared-windows.out" 2> "$tmp/shared-windows.err"; then
	fail "windows sharedlib Ninja lowering unexpectedly succeeded"
fi
contains "$tmp/shared-windows.err" "has Graph IR artifacts for runtime .dll and import .lib"
contains "$tmp/shared-windows.err" "Ninja lowering for platform 'windows' is deferred"
contains "$tmp/shared-windows.err" "docs/windows-artifact-graph-ir.md"

if command -v ninja >/dev/null 2>&1; then
	if [ "$host_windows" -eq 0 ]; then
		"$qstar" --file "$c_app/qstar.lua" -G ninja build //:app --progress off > "$tmp/c-app-build.out" 2> "$tmp/c-app-build.err"
		contains "$tmp/c-app-build.out" "backend ninja"
		contains "$tmp/c-app-build.out" "status ok"
		test -f "$c_app/build/qstar/out/___app/app" || fail "c-app ninja executable missing"
		"$qstar" --file "$c_app/qstar.lua" -G ninja test //:unit > "$tmp/c-app-test.out" 2> "$tmp/c-app-test.err"
		contains "$tmp/c-app-test.out" "backend ninja"
		contains "$tmp/c-app-test.out" "test_result label=//:unit status=pass"
	else
		"$qstar" --file "$c_app/qstar.lua" -G ninja build //:core --progress off > "$tmp/c-app-build.out" 2> "$tmp/c-app-build.err"
		contains "$tmp/c-app-build.out" "backend ninja"
		contains "$tmp/c-app-build.out" "status ok"
		printf 'qstar-ninja-backend-parity: c-app executable runtime skipped reason=windows-explicit-exe-required\n'
	fi
	"$qstar" --file "$c_app/qstar.lua" -G ninja install //:core --prefix "$tmp/c-app-prefix" > "$tmp/c-app-install.out" 2> "$tmp/c-app-install.err"
	contains "$tmp/c-app-install.out" "backend ninja"
	test -f "$tmp/c-app-prefix/lib/libcore.a" || fail "c-app ninja install lib missing"
	test -f "$tmp/c-app-prefix/include/corpus.h" || fail "c-app ninja install header missing"
	test ! -f "$c_app/.ninja_log" || fail "c-app ninja wrote package root .ninja_log"
	test ! -f "$c_app/.ninja_deps" || fail "c-app ninja wrote package root .ninja_deps"

	if [ "$host_windows" -eq 0 ]; then
		"$qstar" --file "$generated/qstar.lua" -G ninja build //:all --progress off > "$tmp/generated-build.out" 2> "$tmp/generated-build.err"
		contains "$tmp/generated-build.out" "backend ninja"
		contains "$tmp/generated-build.out" "Configuring generated config.h"
		contains "$tmp/generated-build.out" "Generating generated value.c"
		contains "$tmp/generated-build.out" "Running generated smoke"
		contains "$tmp/generated-build.out" "run_expect label=//:smoke status=matched contains=GENERATED-OK"
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
	else
		"$qstar" --file "$generated/qstar.lua" -G ninja build //:make_value --progress off > "$tmp/generated-build.out" 2> "$tmp/generated-build.err"
		contains "$tmp/generated-build.out" "backend ninja"
		contains "$tmp/generated-build.out" "Generating generated value.c"
		contains "$tmp/generated-build.out" "status ok"
		test -f "$generated/build/qstar/generated/config.h" || fail "generated ninja config missing"
		test -f "$generated/build/qstar/generated/value.c" || fail "generated ninja source missing"
		printf 'qstar-ninja-backend-parity: generated executable runtime skipped reason=windows-explicit-exe-required\n'
	fi
	test ! -f "$generated/.ninja_log" || fail "generated ninja wrote package root .ninja_log"
	test ! -f "$generated/.ninja_deps" || fail "generated ninja wrote package root .ninja_deps"

	if [ "$host_windows" -eq 1 ]; then
		"$qstar" --file "$object_bridge/qstar.lua" -G ninja build //:objc_static --progress off > "$tmp/object-bridge-static-build.out" 2> "$tmp/object-bridge-static-build.err"
		contains "$tmp/object-bridge-static-build.out" "backend ninja"
		contains "$tmp/object-bridge-static-build.out" "status ok"
		printf 'qstar-ninja-backend-parity: object bridge executable runtime skipped reason=windows-explicit-exe-required\n'
	else
		"$qstar" --file "$object_bridge/qstar.lua" -G ninja build //:all --progress off > "$tmp/object-bridge-build.out" 2> "$tmp/object-bridge-build.err"
		contains "$tmp/object-bridge-build.out" "backend ninja"
		contains "$tmp/object-bridge-build.out" "Building Objective-C object AppDelegate.o"
		contains "$tmp/object-bridge-build.out" "status ok"
		test -f "$object_bridge/build/qstar/out/___app/app" || fail "object bridge ninja executable missing"
		test -f "$object_bridge/$object_shared_artifact" || fail "object bridge ninja sharedlib missing"
		"$object_bridge/build/qstar/out/___app/app"
	fi
	test -f "$object_bridge/build/qstar/generated/objc/AppDelegate.o" || fail "object bridge ninja generated object missing"
	test -f "$object_bridge/build/qstar/out/___objc_static/libobjc_static.a" || fail "object bridge ninja staticlib missing"
	test ! -f "$object_bridge/.ninja_log" || fail "object bridge ninja wrote package root .ninja_log"
	test ! -f "$object_bridge/.ninja_deps" || fail "object bridge ninja wrote package root .ninja_deps"

	"$qstar" --file "$tmp/link-input/qstar.lua" -G ninja build //:app --progress off > "$tmp/link-input-ninja-build.out" 2> "$tmp/link-input-ninja-build.err"
	contains "$tmp/link-input-ninja-build.out" "backend ninja"
	contains "$tmp/link-input-ninja-build.out" "status ok"
	test -f "$tmp/link-input/$link_input_artifact" || fail "link_inputs ninja executable missing"
	"$tmp/link-input/$link_input_artifact"

	if [ "$host_windows" -eq 0 ]; then
		"$qstar" --file "$tmp/shared/qstar.lua" -G ninja build //:plugin_app --progress off > "$tmp/shared-build.out" 2> "$tmp/shared-build.err"
		contains "$tmp/shared-build.out" "backend ninja"
		contains "$tmp/shared-build.out" "status ok"
		test -f "$tmp/shared/$shared_artifact" || fail "sharedlib ninja artifact missing"
		test -f "$tmp/shared/build/qstar/out/___plugin_app/plugin_app" || fail "sharedlib ninja app missing"
		"$tmp/shared/build/qstar/out/___plugin_app/plugin_app"
		"$qstar" --file "$tmp/shared/qstar.lua" -G ninja test //:plugin_test > "$tmp/shared-test.out" 2> "$tmp/shared-test.err"
		contains "$tmp/shared-test.out" "backend ninja"
		contains "$tmp/shared-test.out" "test_result label=//:plugin_test status=pass"
		"$qstar" --file "$tmp/shared/qstar.lua" -G ninja stage //:shared_bundle > "$tmp/shared-stage.out" 2> "$tmp/shared-stage.err"
		contains "$tmp/shared-stage.out" "backend ninja"
		contains "$tmp/shared-stage.out" "stage_file src=$shared_artifact dst=stage/shared/lib/plugin.shared mode=copy"
		test -f "$tmp/shared/stage/shared/lib/plugin.shared" || fail "sharedlib ninja stage artifact missing"
		"$qstar" --file "$tmp/shared/qstar.lua" -G ninja install //:plugin --prefix "$tmp/shared-prefix" > "$tmp/shared-install.out" 2> "$tmp/shared-install.err"
		contains "$tmp/shared-install.out" "backend ninja"
		test -f "$tmp/shared-prefix/lib/$(basename "$shared_artifact")" || fail "sharedlib ninja install artifact missing"
		contains "$tmp/shared/build/qstar/install/manifest.json" "\"role\":\"sharedlib\""
		test ! -f "$tmp/shared/.ninja_log" || fail "sharedlib ninja wrote package root .ninja_log"
		test ! -f "$tmp/shared/.ninja_deps" || fail "sharedlib ninja wrote package root .ninja_deps"
	else
		printf 'qstar-ninja-backend-parity: sharedlib runtime skipped reason=windows-sharedlib-deferred\n'
	fi
else
	printf 'qstar-ninja-backend-parity: ninja runtime checks skipped reason=ninja-not-found\n'
fi

printf 'qstar-ninja-backend-parity: passed\n'
