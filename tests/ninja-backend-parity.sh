#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-ninja-backend-parity.$$
c_app=tests/corpus/c-app
generated=tests/corpus/generated
object_bridge=tests/projects/object-artifact-bridge
workflow=tests/projects/generic-command-artifact-workflow
host=$(uname -s 2>/dev/null || printf unknown)
host_windows=0
case "$host" in
MINGW*|MSYS*|CYGWIN*)
	host_windows=1
	;;
esac
exe_suffix=
if [ "$host_windows" -eq 1 ]; then
	exe_suffix=.exe
fi
c_app_artifact="build/qstar/out/___app/app$exe_suffix"

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
	rm -rf "$c_app/build" "$c_app/stage" "$c_app/exports" "$generated/build" "$generated/stage"
	rm -rf "$object_bridge/build" "$object_bridge/stage"
	rm -rf "$workflow/build" "$workflow/stage" "$workflow/generated" "$workflow/exports"
	rm -f "$c_app/.ninja_log" "$c_app/.ninja_deps"
	rm -f "$generated/.ninja_log" "$generated/.ninja_deps"
	rm -f "$object_bridge/.ninja_log" "$object_bridge/.ninja_deps"
	rm -f "$workflow/.ninja_log" "$workflow/.ninja_deps"
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
contains "$c_app/build/qstar/ninja/build.ninja" "description = Linking C executable $c_app_artifact"
contains "$c_app/build/qstar/compile_commands.json" "src/core.c"
contains "$c_app/build/qstar/compile_commands.json" "src/main.c"
"$qstar" --file "$c_app/qstar.lua" action-log //:app:link:0 > "$tmp/c-app-link-log.out" 2> "$tmp/c-app-link-log.err"
contains "$tmp/c-app-link-log.out" "qstar action-log v1"
contains "$tmp/c-app-link-log.out" "backend=ninja"
contains "$tmp/c-app-link-log.out" "description='Linking C executable $c_app_artifact'"
"$qstar" --file "$c_app/qstar.lua" replay //:app:link:0 > "$tmp/c-app-link-replay.out" 2> "$tmp/c-app-link-replay.err"
contains "$tmp/c-app-link-replay.out" "qstar replay v1"
contains "$tmp/c-app-link-replay.out" "description='Linking C executable $c_app_artifact'"
contains "$tmp/c-app-link-replay.out" "$c_app_artifact"

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
"$qstar" --file "$object_bridge/qstar.lua" list-targets --format json \
	> "$tmp/object-bridge-list-targets.out" 2> "$tmp/object-bridge-list-targets.err"
contains "$tmp/object-bridge-list-targets.out" "\"generated_action_count\":1"
contains "$tmp/object-bridge-list-targets.out" "\"label\":\"//:objc_objects\""
contains "$tmp/object-bridge-list-targets.out" "\"kind\":\"objectlib\""
contains "$tmp/object-bridge-list-targets.out" "\"sources\":[\"build/qstar/generated/objc/AppDelegate.o\"]"
contains "$tmp/object-bridge-list-targets.out" "\"compile_context\":\"own\""
contains "$tmp/object-bridge-list-targets.out" "\"objects\":[\"//:objc_objects\"]"
contains "$tmp/object-bridge-list-targets.out" "\"output_artifacts\":[{\"path\":\"build/qstar/generated/objc/AppDelegate.o\",\"group\":\"objects\",\"format\":\"object\""
"$qstar" --file "$object_bridge/qstar.lua" query //:objc_objects \
	> "$tmp/object-bridge-query.out" 2> "$tmp/object-bridge-query.err"
contains "$tmp/object-bridge-query.out" "kind objectlib"
contains "$tmp/object-bridge-query.out" "sources [build/qstar/generated/objc/AppDelegate.o]"
contains "$tmp/object-bridge-query.out" "compile_context own"
"$qstar" --file "$object_bridge/qstar.lua" query //:app \
	> "$tmp/object-bridge-app-query.out" 2> "$tmp/object-bridge-app-query.err"
contains "$tmp/object-bridge-app-query.out" "kind exe"
contains "$tmp/object-bridge-app-query.out" "objects [//:objc_objects]"
if [ "$host_windows" -eq 1 ]; then
	"$qstar" --file "$object_bridge/qstar.lua" dry-run //:app \
		> "$tmp/object-bridge-dry.out" 2> "$tmp/object-bridge-dry.err"
	contains "$tmp/object-bridge-dry.out" "generated_artifact output=build/qstar/generated/objc/AppDelegate.o group=objects format=object"
	contains "$tmp/object-bridge-dry.out" "dry_run_target //:objc_objects"
	contains "$tmp/object-bridge-dry.out" "dry_run_step id=//:objc_objects:link-input:0"
	"$qstar" --file "$object_bridge/qstar.lua" dry-run //:objc_static \
		> "$tmp/object-bridge-static-dry.out" 2> "$tmp/object-bridge-static-dry.err"
	contains "$tmp/object-bridge-static-dry.out" "dry_run_step id=//:objc_objects:link-input:0"
	contains "$tmp/object-bridge-static-dry.out" "dry_run_step id=//:objc_objects:compile-objects:0"
	"$qstar" --file "$object_bridge/qstar.lua" emit-ninja //:app \
		> "$tmp/object-bridge-emit.out" 2> "$tmp/object-bridge-emit.err"
	contains "$tmp/object-bridge-emit.out" "ninja_file build/qstar/ninja/build.ninja"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "qstar_action_id = //:objc_object:generate:0"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "description = Building Objective-C object AppDelegate.o"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "$c_app_artifact: qstar_link build/qstar/out/___app/obj0.o build/qstar/generated/objc/AppDelegate.o"
	"$qstar" --file "$object_bridge/qstar.lua" emit-ninja //:objc_static \
		> "$tmp/object-bridge-static-emit.out" 2> "$tmp/object-bridge-static-emit.err"
	contains "$object_bridge/build/qstar/ninja/build.ninja" "build/qstar/out/___objc_static/libobjc_static.a: qstar_archive build/qstar/generated/objc/AppDelegate.o"
else
	"$qstar" --file "$object_bridge/qstar.lua" dry-run //:all > "$tmp/object-bridge-dry.out" 2> "$tmp/object-bridge-dry.err"
	contains "$tmp/object-bridge-dry.out" "generated_artifact output=build/qstar/generated/objc/AppDelegate.o group=objects format=object"
	contains "$tmp/object-bridge-dry.out" "dry_run_target //:objc_objects"
	contains "$tmp/object-bridge-dry.out" "dry_run_step id=//:objc_objects:link-input:0"
	contains "$tmp/object-bridge-dry.out" "dry_run_step id=//:objc_objects:compile-objects:0"
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

"$qstar" --file "$workflow/qstar.lua" emit-ninja //:artifact_smoke > "$tmp/workflow-emit.out" 2> "$tmp/workflow-emit.err"
contains "$tmp/workflow-emit.out" "ninja_file build/qstar/ninja/build.ninja"
contains "$workflow/build/qstar/ninja/build.ninja" "qstar_action_id = //:payload_artifact:generate:0"
contains "$workflow/build/qstar/ninja/build.ninja" "qstar_action_id = //:artifact_smoke:run:0"
contains "$workflow/build/qstar/ninja/build.ninja" "description = Transforming payload artifact"
contains "$workflow/build/qstar/ninja/build.ninja" "description = Checking workflow artifact inputs"
"$qstar" --file "$workflow/qstar.lua" action-log //:payload_artifact:generate:0 > "$tmp/workflow-transform-log.out" 2> "$tmp/workflow-transform-log.err"
contains "$tmp/workflow-transform-log.out" "qstar action-log v1"
contains "$tmp/workflow-transform-log.out" "backend=ninja"
contains "$tmp/workflow-transform-log.out" "description='Transforming payload artifact'"
"$qstar" --file "$workflow/qstar.lua" replay //:payload_artifact:generate:0 > "$tmp/workflow-transform-replay.out" 2> "$tmp/workflow-transform-replay.err"
contains "$tmp/workflow-transform-replay.out" "qstar replay v1"
contains "$tmp/workflow-transform-replay.out" "tools/transform-artifact.sh fixtures/payload.artifact generated/artifacts/payload.artifact"

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

qstar.command "install" {
  options = {
    out = qstar.param.path {
      default = "exports/shared",
    },
  },
  steps = {
    qstar.step.export_stage("//:shared_bundle", {
      to = qstar.param("out"),
    }),
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
"$qstar" --file "$tmp/shared/qstar.lua" --qstar-internal-platform windows \
	emit-ninja //:plugin_app > "$tmp/shared-windows.out" 2> "$tmp/shared-windows.err"
contains "$tmp/shared/build/qstar/ninja/build.ninja" \
	"build/qstar/out/___plugin/plugin.dll build/qstar/out/___plugin/plugin.lib: qstar_link"
contains "$tmp/shared/build/qstar/ninja/build.ninja" \
	"build/qstar/out/___plugin_app/plugin_app.exe: qstar_link build/qstar/out/___plugin_app/obj0.o build/qstar/out/___plugin/plugin.lib"
contains "$tmp/shared/build/qstar/ninja/build.ninja" "-Wl,--out-implib,build/qstar/out/___plugin/plugin.lib"

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
	"$qstar" --file "$c_app/qstar.lua" -G ninja install --out exports/install > "$tmp/c-app-install.out" 2> "$tmp/c-app-install.err"
	contains "$tmp/c-app-install.out" "backend ninja"
	test -f "$c_app/exports/install/lib/libcore.a" || fail "c-app ninja command export lib missing"
	test -f "$c_app/exports/install/include/corpus.h" || fail "c-app ninja command export header missing"
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

	"$qstar" --file "$workflow/qstar.lua" -G ninja build //:artifact_smoke --progress off > "$tmp/workflow-build.out" 2> "$tmp/workflow-build.err"
	contains "$tmp/workflow-build.out" "backend ninja"
	contains "$tmp/workflow-build.out" "Checking workflow artifact inputs"
	contains "$tmp/workflow-build.out" "run_expect label=//:artifact_smoke status=matched contains=WORKFLOW_OK"
	contains "$tmp/workflow-build.out" "status ok"
	test -f "$workflow/generated/artifacts/payload.artifact" || fail "workflow ninja transform output missing"
	test -f "$workflow/stage/workflow/artifacts/payload.artifact" || fail "workflow ninja stage artifact missing"
	"$qstar" --file "$workflow/qstar.lua" -G ninja --progress off workflow --out exports/ninja --mode full > "$tmp/workflow-command.out" 2> "$tmp/workflow-command.err"
	contains "$tmp/workflow-command.out" "backend ninja"
	contains "$tmp/workflow-command.out" "command_step 4/4 kind=export_stage label=//:workflow_layout"
	contains "$tmp/workflow-command.out" "command_export_stage label=//:workflow_layout to=exports/ninja mode=copy"
	test -f "$workflow/exports/ninja/artifacts/payload.artifact" || fail "workflow ninja export_stage artifact missing"
	contains "$workflow/generated/check/project-command.txt" "mode=full"
	"$qstar" --file "$workflow/qstar.lua" -G ninja commands --format json > "$tmp/workflow-commands-json.out" 2> "$tmp/workflow-commands-json.err"
	contains "$tmp/workflow-commands-json.out" "\"command_count\":4"
	contains "$tmp/workflow-commands-json.out" "\"name\":\"install\""
	contains "$tmp/workflow-commands-json.out" "install-local"
	contains "$tmp/workflow-commands-json.out" "\"label\":\"//:install_layout\""
	contains "$tmp/workflow-commands-json.out" "\"name\":\"package-local\""
	contains "$tmp/workflow-commands-json.out" "\"label\":\"//:package_layout\""
	contains "$tmp/workflow-commands-json.out" "\"name\":\"export-local\""
	"$qstar" --file "$workflow/qstar.lua" -G ninja --progress off install --out exports/ninja-install > "$tmp/workflow-install.out" 2> "$tmp/workflow-install.err"
	contains "$tmp/workflow-install.out" "backend ninja"
	contains "$tmp/workflow-install.out" "command_step 1/1 kind=export_stage label=//:install_layout"
	contains "$tmp/workflow-install.out" "command_export_stage label=//:install_layout to=exports/ninja-install mode=copy"
	test -f "$workflow/exports/ninja-install/share/generic-command-artifact-workflow/payload.artifact" || fail "workflow ninja install command artifact missing"
	"$qstar" --file "$workflow/qstar.lua" -G ninja --progress off package-local --out exports/ninja-package > "$tmp/workflow-package.out" 2> "$tmp/workflow-package.err"
	contains "$tmp/workflow-package.out" "backend ninja"
	contains "$tmp/workflow-package.out" "command_step 3/3 kind=export_stage label=//:package_layout"
	contains "$tmp/workflow-package.out" "command_export_stage label=//:package_layout to=exports/ninja-package mode=copy"
	test -f "$workflow/exports/ninja-package/payload/payload.artifact" || fail "workflow ninja package command artifact missing"
	"$qstar" --file "$workflow/qstar.lua" -G ninja --progress off export-local --out exports/ninja-direct > "$tmp/workflow-export-local.out" 2> "$tmp/workflow-export-local.err"
	contains "$tmp/workflow-export-local.out" "backend ninja"
	contains "$tmp/workflow-export-local.out" "command_export_stage label=//:workflow_layout to=exports/ninja-direct mode=copy"
	test -f "$workflow/exports/ninja-direct/artifacts/payload.artifact" || fail "workflow ninja export-local artifact missing"
	test ! -f "$workflow/.ninja_log" || fail "workflow ninja wrote package root .ninja_log"
	test ! -f "$workflow/.ninja_deps" || fail "workflow ninja wrote package root .ninja_deps"

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
		"$qstar" --file "$tmp/shared/qstar.lua" -G ninja install --out exports/shared > "$tmp/shared-install.out" 2> "$tmp/shared-install.err"
		contains "$tmp/shared-install.out" "backend ninja"
		test -f "$tmp/shared/exports/shared/lib/plugin.shared" || fail "sharedlib ninja command export artifact missing"
		contains "$tmp/shared/build/qstar/stage/___shared_bundle/manifest.json" "\"producer\":\"//:plugin\""
		test ! -f "$tmp/shared/.ninja_log" || fail "sharedlib ninja wrote package root .ninja_log"
		test ! -f "$tmp/shared/.ninja_deps" || fail "sharedlib ninja wrote package root .ninja_deps"
		else
			"$qstar" --file "$tmp/shared/qstar.lua" -G ninja build //:plugin_app --progress off > "$tmp/shared-build.out" 2> "$tmp/shared-build.err"
			contains "$tmp/shared-build.out" "backend ninja"
			contains "$tmp/shared-build.out" "status ok"
			test -f "$tmp/shared/build/qstar/out/___plugin/plugin.dll" || fail "windows sharedlib ninja runtime missing"
			test -f "$tmp/shared/build/qstar/out/___plugin/plugin.lib" || fail "windows sharedlib ninja import lib missing"
			test -f "$tmp/shared/build/qstar/out/___plugin_app/plugin_app.exe" || fail "windows sharedlib ninja app missing"
			"$qstar" --file "$tmp/shared/qstar.lua" -G ninja action-log //:plugin_app:link:0 > "$tmp/shared-app-log.out" 2> "$tmp/shared-app-log.err"
			contains "$tmp/shared-app-log.out" "build/qstar/out/___plugin/plugin.lib"
			printf 'qstar-ninja-backend-parity: sharedlib runtime skipped reason=windows-dll-search-path\n'
		fi
else
	printf 'qstar-ninja-backend-parity: ninja runtime checks skipped reason=ninja-not-found\n'
fi

printf 'qstar-ninja-backend-parity: passed\n'
