#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
baseline=${QSTAR_WINDOWS_EXECUTION_BASELINE:-msys2-ucrt64-gcc}
tmp=${TMPDIR:-/tmp}/qstar-windows-execution.$$
corpus=tests/corpus/windows-execution
build_dir=build/qstar
artifact_dir=${QSTAR_WINDOWS_EXECUTION_ARTIFACT_DIR:-}

fail() {
	printf 'qstar-windows-execution: %s\n' "$1" >&2
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

run_artifact() {
	artifact=$1
	out=$2
	test -x "$artifact" || fail "missing executable artifact $artifact"
	"$artifact" > "$out" 2>&1
}

copy_if_exists() {
	src=$1
	dst=$2
	if test -e "$src"; then
		rm -rf "$dst"
		mkdir -p "$(dirname "$dst")"
		cp -R "$src" "$dst"
	fi
}

collect_failure_artifacts() {
	rc=$1
	if test "$rc" -eq 0 || test -z "$artifact_dir"; then
		return 0
	fi
	mkdir -p "$artifact_dir/tmp" "$artifact_dir/corpus"
	printf 'status=fail script=windows-execution-corpus rc=%s tmp=%s\n' "$rc" "$tmp" \
		> "$artifact_dir/failure.status"
	if test -d "$tmp"; then
		find "$tmp" -maxdepth 1 -type f -exec cp {} "$artifact_dir/tmp/" \;
	fi
	copy_if_exists "$corpus/$build_dir" "$artifact_dir/corpus/$build_dir"
	copy_if_exists "$corpus/stage" "$artifact_dir/corpus/stage"
	copy_if_exists "$corpus/.ninja_log" "$artifact_dir/corpus/.ninja_log"
	copy_if_exists "$corpus/.ninja_deps" "$artifact_dir/corpus/.ninja_deps"
}

rm -rf "$tmp"
mkdir -p "$tmp"
rm -rf "$corpus/build" "$corpus/stage"
rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps"
trap 'rc=$?; trap - EXIT HUP INT TERM; collect_failure_artifacts "$rc"; rm -rf "$tmp"; rm -rf "$corpus/build" "$corpus/stage"; rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps"; exit "$rc"' EXIT HUP INT TERM

host=$(uname -s 2>/dev/null || printf unknown)
case "$host" in
MINGW*|MSYS*|CYGWIN*)
	mode=native-windows-execution
	;;
*)
	mode=contract-execution
	;;
esac

printf 'qstar-windows-execution: host=%s mode=%s baseline=%s\n' "$host" "$mode" "$baseline"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows check \
	> "$tmp/check.out" 2> "$tmp/check.err"
contains "$tmp/check.out" "status ok"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows dry-run //:bridge_app \
	> "$tmp/bridge-dry.out" 2> "$tmp/bridge-dry.err"
contains "$tmp/bridge-dry.out" "Building external object bridge_payload.o"
contains "$tmp/bridge-dry.out" "build/qstar/generated/bridge/bridge_payload.o"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --progress plain \
	build //:hello > "$tmp/hello-build.out" 2> "$tmp/hello-build.err"
contains "$tmp/hello-build.out" "status ok"
run_artifact "$corpus/$build_dir/out/___hello/hello.exe" "$tmp/hello-run.out"
contains "$tmp/hello-run.out" "windows-execution hello"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --progress plain \
	build //:hello_smoke > "$tmp/hello-smoke.out" 2> "$tmp/hello-smoke.err"
contains "$tmp/hello-smoke.out" "run_target label=//:hello_smoke command=argv"
contains "$tmp/hello-smoke.out" "run_expect label=//:hello_smoke status=matched contains=windows-execution hello"
contains "$tmp/hello-smoke.out" "status ok"
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	action-log //:hello_smoke:run:0 > "$tmp/hello-smoke-log.out" \
	2> "$tmp/hello-smoke-log.err"
contains "$tmp/hello-smoke-log.out" "qstar action-log v1"
contains "$tmp/hello-smoke-log.out" "qstar-action-log v2"
case "$mode" in
native-windows-execution)
	contains "$tmp/hello-smoke-log.out" "windows_command_line="
	;;
esac
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	replay //:hello_smoke:run:0 > "$tmp/hello-smoke-replay.out" \
	2> "$tmp/hello-smoke-replay.err"
contains "$tmp/hello-smoke-replay.out" "qstar replay v1"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --progress plain \
	build //:app > "$tmp/app-build.out" 2> "$tmp/app-build.err"
contains "$tmp/app-build.out" "status ok"
test -f "$corpus/$build_dir/out/___core/libwinexec_core.a" ||
	fail "static library artifact missing"
contains "$corpus/$build_dir/compile_commands.json" "src/app.c"
run_artifact "$corpus/$build_dir/out/___app/app.exe" "$tmp/app-run.out"
contains "$tmp/app-run.out" "windows-execution app core=42"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --progress plain \
	build //:response_probe > "$tmp/response-build.out" 2> "$tmp/response-build.err"
contains "$tmp/response-build.out" "response_file id=//:response_probe:compile:0"
contains "$tmp/response-build.out" "status ok"
test -f "$corpus/$build_dir/rsp/___response_probe_compile_0.rsp" ||
	fail "response probe compile response file missing"
run_artifact "$corpus/$build_dir/out/___response_probe/response_probe.exe" \
	"$tmp/response-run.out"
contains "$tmp/response-run.out" "windows-execution response-probe"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --progress plain \
	build //:bridge_app > "$tmp/bridge-build.out" 2> "$tmp/bridge-build.err"
contains "$tmp/bridge-build.out" "Building external object bridge_payload.o"
contains "$tmp/bridge-build.out" "status ok"
test -f "$corpus/$build_dir/generated/bridge/bridge_payload.o" ||
	fail "generated object bridge output missing"
run_artifact "$corpus/$build_dir/out/___bridge_app/bridge_app.exe" \
	"$tmp/bridge-run.out"
contains "$tmp/bridge-run.out" "windows-execution bridge=77"

prefix="$tmp/prefix"
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows install //:app \
	--prefix "$prefix" > "$tmp/install-app.out" 2> "$tmp/install-app.err"
contains "$tmp/install-app.out" "install_file src=build/qstar/out/___app/app.exe"
contains "$tmp/install-app.out" "role=exe"
test -f "$prefix/bin/app.exe" || fail "installed app.exe missing"
install_manifest="$corpus/$build_dir/install/manifest.json"
contains "$install_manifest" "\"schema\":\"qstar-install-manifest-v2\""
contains "$install_manifest" "\"prefix\":\"$prefix\""
contains "$install_manifest" "\"role\":\"exe\""
contains "$install_manifest" "\"src\":\"build/qstar/out/___app/app.exe\""
contains "$install_manifest" "\"dst\":\"$prefix/bin/app.exe\""
not_contains "$install_manifest" "\\"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows install //:core \
	--prefix "$prefix" > "$tmp/install-core.out" 2> "$tmp/install-core.err"
contains "$tmp/install-core.out" "install_file src=build/qstar/out/___core/libwinexec_core.a"
contains "$tmp/install-core.out" "role=staticlib"
test -f "$prefix/lib/libwinexec_core.a" ||
	fail "installed static library missing"
contains "$install_manifest" "\"role\":\"staticlib\""
contains "$install_manifest" "\"src\":\"build/qstar/out/___core/libwinexec_core.a\""
contains "$install_manifest" "\"dst\":\"$prefix/lib/libwinexec_core.a\""
not_contains "$install_manifest" "\\"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows install //:bridge_app \
	--prefix "$prefix" > "$tmp/install-bridge-app.out" 2> "$tmp/install-bridge-app.err"
contains "$tmp/install-bridge-app.out" "install_file src=build/qstar/out/___bridge_app/bridge_app.exe"
contains "$tmp/install-bridge-app.out" "role=exe"
test -f "$prefix/bin/bridge_app.exe" || fail "installed bridge_app.exe missing"
contains "$install_manifest" "\"role\":\"exe\""
contains "$install_manifest" "\"src\":\"build/qstar/out/___bridge_app/bridge_app.exe\""
contains "$install_manifest" "\"dst\":\"$prefix/bin/bridge_app.exe\""
not_contains "$install_manifest" "\\"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows stage //:layout \
	> "$tmp/stage-layout.out" 2> "$tmp/stage-layout.err"
contains "$tmp/stage-layout.out" "stage_file src=build/qstar/out/___app/app.exe"
contains "$tmp/stage-layout.out" "stage_file src=build/qstar/out/___core/libwinexec_core.a"
contains "$tmp/stage-layout.out" "stage_file src=build/qstar/out/___bridge_app/bridge_app.exe"
contains "$tmp/stage-layout.out" "stage_file src=build/qstar/generated/bridge/bridge_payload.o"
contains "$tmp/stage-layout.out" "kind=custom_output producer=//:bridge_object"
contains "$tmp/stage-layout.out" "status ok"
test -f "$corpus/stage/windows-execution/bin/app.exe" ||
	fail "staged app.exe missing"
test -f "$corpus/stage/windows-execution/lib/libwinexec_core.a" ||
	fail "staged static library missing"
test -f "$corpus/stage/windows-execution/bin/bridge_app.exe" ||
	fail "staged bridge_app.exe missing"
test -f "$corpus/stage/windows-execution/objects/bridge_payload.o" ||
	fail "staged generated object bridge output missing"
stage_manifest="$corpus/$build_dir/stage/___layout/manifest.json"
contains "$stage_manifest" "\"schema\":\"qstar-stage-manifest-v2\""
contains "$stage_manifest" "\"root\":\"stage/windows-execution\""
contains "$stage_manifest" "\"src\":\"build/qstar/generated/bridge/bridge_payload.o\""
contains "$stage_manifest" "\"dst\":\"stage/windows-execution/objects/bridge_payload.o\""
contains "$stage_manifest" "\"kind\":\"custom_output\""
contains "$stage_manifest" "\"producer\":\"//:bridge_object\""
not_contains "$stage_manifest" "\\"

if command -v ninja >/dev/null 2>&1; then
	rm -rf "$corpus/$build_dir" "$corpus/stage"
	rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps"
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
		-G ninja --progress plain build //:all \
		> "$tmp/ninja-all.out" 2> "$tmp/ninja-all.err"
	contains "$tmp/ninja-all.out" "backend ninja"
	contains "$tmp/ninja-all.out" "run_target label=//:hello_smoke command=argv"
	contains "$tmp/ninja-all.out" "run_expect label=//:hello_smoke status=matched contains=windows-execution hello"
	contains "$tmp/ninja-all.out" "status ok"
	contains "$corpus/$build_dir/ninja/build.ninja" "command = sh tools/build-object.sh"
	run_artifact "$corpus/$build_dir/out/___hello/hello.exe" "$tmp/ninja-hello-run.out"
	contains "$tmp/ninja-hello-run.out" "windows-execution hello"
	test -f "$corpus/$build_dir/out/___core/libwinexec_core.a" ||
		fail "ninja static library artifact missing"
	test -f "$corpus/$build_dir/rsp/___response_probe_compile_0.rsp" ||
		fail "ninja response probe compile response file missing"
	run_artifact "$corpus/$build_dir/out/___response_probe/response_probe.exe" \
		"$tmp/ninja-response-run.out"
	contains "$tmp/ninja-response-run.out" "windows-execution response-probe"
	test -f "$corpus/$build_dir/generated/bridge/bridge_payload.o" ||
		fail "ninja generated object bridge output missing"
	run_artifact "$corpus/$build_dir/out/___bridge_app/bridge_app.exe" \
		"$tmp/ninja-bridge-run.out"
	contains "$tmp/ninja-bridge-run.out" "windows-execution bridge=77"
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
		-G ninja action-log //:bridge_object:generate:0 \
		> "$tmp/ninja-bridge-log.out" 2> "$tmp/ninja-bridge-log.err"
	contains "$tmp/ninja-bridge-log.out" "qstar action-log v1"
	contains "$tmp/ninja-bridge-log.out" "backend=ninja"
	contains "$tmp/ninja-bridge-log.out" "argv[0]=tools/build-object.sh"
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
		-G ninja replay //:bridge_object:generate:0 \
		> "$tmp/ninja-bridge-replay.out" 2> "$tmp/ninja-bridge-replay.err"
	contains "$tmp/ninja-bridge-replay.out" "qstar replay v1"
	contains "$tmp/ninja-bridge-replay.out" "tools/build-object.sh"
	ninja_prefix="$tmp/ninja-prefix"
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
		-G ninja install //:app --prefix "$ninja_prefix" \
		> "$tmp/ninja-install-app.out" 2> "$tmp/ninja-install-app.err"
	contains "$tmp/ninja-install-app.out" "backend ninja"
	test -f "$ninja_prefix/bin/app.exe" || fail "ninja installed app.exe missing"
	install_manifest="$corpus/$build_dir/install/manifest.json"
	contains "$install_manifest" "\"role\":\"exe\""
	contains "$install_manifest" "\"dst\":\"$ninja_prefix/bin/app.exe\""
	not_contains "$install_manifest" "\\"
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
		-G ninja install //:core --prefix "$ninja_prefix" \
		> "$tmp/ninja-install-core.out" 2> "$tmp/ninja-install-core.err"
	contains "$tmp/ninja-install-core.out" "backend ninja"
	test -f "$ninja_prefix/lib/libwinexec_core.a" ||
		fail "ninja installed static library missing"
	contains "$install_manifest" "\"role\":\"staticlib\""
	contains "$install_manifest" "\"dst\":\"$ninja_prefix/lib/libwinexec_core.a\""
	not_contains "$install_manifest" "\\"
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
		-G ninja install //:bridge_app --prefix "$ninja_prefix" \
		> "$tmp/ninja-install-bridge-app.out" 2> "$tmp/ninja-install-bridge-app.err"
	contains "$tmp/ninja-install-bridge-app.out" "backend ninja"
	test -f "$ninja_prefix/bin/bridge_app.exe" ||
		fail "ninja installed bridge_app.exe missing"
	contains "$install_manifest" "\"role\":\"exe\""
	contains "$install_manifest" "\"dst\":\"$ninja_prefix/bin/bridge_app.exe\""
	not_contains "$install_manifest" "\\"
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
		-G ninja stage //:layout > "$tmp/ninja-stage-layout.out" \
		2> "$tmp/ninja-stage-layout.err"
	contains "$tmp/ninja-stage-layout.out" "backend ninja"
	contains "$tmp/ninja-stage-layout.out" "stage_file src=build/qstar/generated/bridge/bridge_payload.o"
	contains "$tmp/ninja-stage-layout.out" "kind=custom_output producer=//:bridge_object"
	contains "$tmp/ninja-stage-layout.out" "status ok"
	test -f "$corpus/stage/windows-execution/bin/app.exe" ||
		fail "ninja staged app.exe missing"
	test -f "$corpus/stage/windows-execution/lib/libwinexec_core.a" ||
		fail "ninja staged static library missing"
	test -f "$corpus/stage/windows-execution/bin/bridge_app.exe" ||
		fail "ninja staged bridge_app.exe missing"
	test -f "$corpus/stage/windows-execution/objects/bridge_payload.o" ||
		fail "ninja staged generated object bridge output missing"
	stage_manifest="$corpus/$build_dir/stage/___layout/manifest.json"
	contains "$stage_manifest" "\"src\":\"build/qstar/generated/bridge/bridge_payload.o\""
	contains "$stage_manifest" "\"dst\":\"stage/windows-execution/objects/bridge_payload.o\""
	contains "$stage_manifest" "\"kind\":\"custom_output\""
	contains "$stage_manifest" "\"producer\":\"//:bridge_object\""
	not_contains "$stage_manifest" "\\"
	test ! -f "$corpus/.ninja_log" || fail "ninja wrote root .ninja_log"
	test ! -f "$corpus/.ninja_deps" || fail "ninja wrote root .ninja_deps"
	printf 'qstar-windows-execution: ninja=passed\n'
else
	printf 'qstar-windows-execution: ninja=skipped reason=ninja-not-found\n'
fi

printf 'qstar-windows-execution: passed\n'
