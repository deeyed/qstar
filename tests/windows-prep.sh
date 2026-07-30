#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-windows-prep.$$
corpus=tests/corpus/response-files
artifact_corpus=tests/corpus/windows-artifacts
wide_corpus=tests/corpus/windows-wide-final
build_dir=build/qstar
artifact_dir=${QSTAR_WINDOWS_PREP_ARTIFACT_DIR:-}

if test -z "$artifact_dir" && test -n "${QSTAR_WINDOWS_ALPHA_DIR:-}"; then
	artifact_dir=$QSTAR_WINDOWS_ALPHA_DIR/windows-prep-detail
fi

fail() {
	printf 'qstar-windows-prep: %s\n' "$1" >&2
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
		printf 'status=fail script=windows-prep rc=%s tmp=%s\n' "$rc" "$tmp" \
			> "$artifact_dir/failure.status"
		if test -d "$tmp"; then
			cp -R "$tmp"/. "$artifact_dir/tmp"/
		fi
		if test -d "$corpus/$build_dir"; then
			mkdir -p "$artifact_dir/response-files"
			cp -R "$corpus/$build_dir" "$artifact_dir/response-files/build-qstar"
		fi
		if test -d "$artifact_corpus/$build_dir"; then
			mkdir -p "$artifact_dir/windows-artifacts"
			cp -R "$artifact_corpus/$build_dir" \
				"$artifact_dir/windows-artifacts/build-qstar"
		fi
		printf 'qstar-windows-prep: failed rc=%s detail=%s\n' "$rc" \
			"$artifact_dir" >&2
	fi
	rm -rf "$tmp"
	rm -rf "$corpus/build" "$corpus/stage" "$artifact_corpus/build" \
		"$artifact_corpus/stage"
	rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps" \
		"$artifact_corpus/.ninja_log" "$artifact_corpus/.ninja_deps"
	exit "$rc"
}

rm -rf "$tmp"
mkdir -p "$tmp"
rm -rf "$corpus/build" "$corpus/stage" "$artifact_corpus/build" "$artifact_corpus/stage"
rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps"
rm -f "$artifact_corpus/.ninja_log" "$artifact_corpus/.ninja_deps"
trap finish EXIT HUP INT TERM

wide_project=$tmp/windows-wide-final
mkdir -p "$wide_project"
cp -R "$wide_corpus"/. "$wide_project"/
mkdir -p "$wide_project/objects" "$wide_project/tools"
printf '#!/bin/sh\nexit 0\n' > "$wide_project/tools/fake-link.exe"
chmod +x "$wide_project/tools/fake-link.exe"
i=0
while test "$i" -lt 1000; do
	printf 'windows wide object %s\n' "$i" \
		> "$wide_project/objects/object-$(printf '%04d' "$i").obj"
	i=$((i + 1))
done
"$qstar" --file "$wide_project/qstar.lua" \
	-D object-count=1000 --qstar-internal-platform windows \
	-B build/parity dry-run //:app \
	> "$tmp/windows-wide-stella-dry.out" \
	2> "$tmp/windows-wide-stella-dry.err"
"$qstar" --file "$wide_project/qstar.lua" \
	-D object-count=1000 --qstar-internal-platform windows \
	-B build/parity -G ninja dry-run //:app \
	> "$tmp/windows-wide-ninja-dry.out" \
	2> "$tmp/windows-wide-ninja-dry.err"
for output in "$tmp/windows-wide-stella-dry.out" \
	"$tmp/windows-wide-ninja-dry.out"; do
	contains "$output" "logical_argc=1002"
	contains "$output" "object_count=1000"
	contains "$output" "input_count=1000"
	contains "$output" "response_style=msvc"
	contains "$output" "response=skeleton"
	contains "$output" "exec_argc=2"
done
stella_digest=$(sed -n \
	's/.* response_digest=\([^ ]*\).*/\1/p' \
	"$tmp/windows-wide-stella-dry.out" | tail -n 1)
ninja_digest=$(sed -n \
	's/.* response_digest=\([^ ]*\).*/\1/p' \
	"$tmp/windows-wide-ninja-dry.out" | tail -n 1)
test -n "$stella_digest" || fail "Windows wide response digest missing"
test "$stella_digest" = "$ninja_digest" ||
	fail "Windows wide Stella/Ninja response digests differ"
"$qstar" --file "$wide_project/qstar.lua" \
	-D object-count=1000 --qstar-internal-platform windows \
	-B build/ninja -G ninja emit-ninja //:app \
	> "$tmp/windows-wide-ninja-emit.out" \
	2> "$tmp/windows-wide-ninja-emit.err"
contains "$wide_project/build/ninja/ninja/build.ninja" \
	"@build/ninja/rsp/___app_link_0.rsp"
printf 'qstar-windows-prep: wide_final objects=1000 style=msvc response_digest=%s status=ok\n' \
	"$stella_digest"

"$qstar" --file "$corpus/qstar.lua" build //:all \
	--progress off > "$tmp/build.out" 2> "$tmp/build.err"
contains "$tmp/build.out" "response_file id=//:app:compile:0"
contains "$tmp/build.out" "style=posix"
contains "$tmp/build.out" "status ok"
test -x "$corpus/$build_dir/out/___app/app" || fail "response corpus app missing"
"$corpus/$build_dir/out/___app/app"
test -f "$corpus/$build_dir/rsp/___app_compile_0.rsp" ||
	fail "response file missing"
contains "$corpus/$build_dir/rsp/___app_compile_0.rsp" "include/very/long/path/segment/019"
contains "$corpus/$build_dir/compile_commands.json" "src/main.c"
not_contains "$corpus/$build_dir/compile_commands.json" "\\"

"$qstar" --file "$corpus/qstar.lua" build //:argv_probe \
	--progress off > "$tmp/argv-probe.out" 2> "$tmp/argv-probe.err"
contains "$tmp/argv-probe.out" "status ok"
test -f "$corpus/$build_dir/generated/argv.txt" ||
	fail "argv probe output missing"
contains "$corpus/$build_dir/generated/argv.txt" "arg1=alpha beta"
contains "$corpus/$build_dir/generated/argv.txt" "arg2=quote ' value"
contains "$corpus/$build_dir/generated/argv.txt" "arg3=semi;colon"
contains "$corpus/$build_dir/generated/argv.txt" 'arg4=dollar$value'

"$qstar" --file "$corpus/qstar.lua" action-log \
	//:app:compile:0 > "$tmp/action-log.out" 2> "$tmp/action-log.err"
contains "$tmp/action-log.out" "qstar action-log v1"
contains "$tmp/action-log.out" "argc=50"
contains "$tmp/action-log.out" "argv[2]=src/main.c"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows dry-run \
	//:windows_app > "$tmp/windows-dry.out" 2> "$tmp/windows-dry.err"
contains "$tmp/windows-dry.out" "response_style=msvc"
contains "$tmp/windows-dry.out" "response=skeleton"
contains "$tmp/windows-dry.out" "response_file=build/qstar/rsp/"
contains "$tmp/windows-dry.out" "/link"
contains "$tmp/windows-dry.out" "/LIBPATH:sdk/lib/um/x64"
contains "$tmp/windows-dry.out" "kernel32.lib"
contains "$tmp/windows-dry.out" "output=build/qstar/out/___windows_app/windows_app.exe"
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows explain \
	//:windows_app > "$tmp/windows-explain.out" 2> "$tmp/windows-explain.err"
contains "$tmp/windows-explain.out" "Linking C executable build/qstar/out/___windows_app/windows_app.exe"
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows list-targets \
	--format json > "$tmp/windows-list.json" 2> "$tmp/windows-list.err"
contains "$tmp/windows-list.json" "\"artifact_name\":\"windows_app.exe\""

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows dry-run \
	//:windows_mapped > "$tmp/windows-mapped-dry.out" 2> "$tmp/windows-mapped-dry.err"
contains "$tmp/windows-mapped-dry.out" "response_style=msvc"
contains "$tmp/windows-mapped-dry.out" "output=build/qstar/out/___windows_mapped/mapped_named.exe"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows dry-run \
	//:windows_static > "$tmp/windows-static-dry.out" 2> "$tmp/windows-static-dry.err"
contains "$tmp/windows-static-dry.out" "final_action=archive"
contains "$tmp/windows-static-dry.out" "output=build/qstar/out/___windows_static/windows_static.lib"
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	--progress off build //:windows_static > "$tmp/windows-static-build.out" \
	2> "$tmp/windows-static-build.err"
contains "$tmp/windows-static-build.out" "response_file id=//:windows_static:compile:0"
contains "$tmp/windows-static-build.out" "status ok"
test -f "$corpus/$build_dir/out/___windows_static/windows_static.lib" ||
	fail "fake Windows static .lib artifact missing"
contains "$corpus/$build_dir/out/___windows_static/windows_static.lib" "fake static library"
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	action-log //:windows_static:archive:0 > "$tmp/windows-static-log.out" \
	2> "$tmp/windows-static-log.err"
contains "$tmp/windows-static-log.out" "argv[0]=tools/fake-lib.sh"
contains "$tmp/windows-static-log.out" "windows_static.lib"
contains "$tmp/windows-static-log.out" "description='Linking C static library build/qstar/out/___windows_static/windows_static.lib'"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	--progress off build //:windows_plugin > "$tmp/windows-shared.out" \
	2> "$tmp/windows-shared.err"
contains "$tmp/windows-shared.out" "status ok"
test -x "$corpus/$build_dir/out/___windows_plugin/windows_plugin.dll" ||
	fail "fake Windows sharedlib runtime .dll missing"
test -f "$corpus/$build_dir/out/___windows_plugin/windows_plugin.lib" ||
	fail "fake Windows sharedlib import .lib missing"
contains "$corpus/$build_dir/out/___windows_plugin/windows_plugin.lib" \
	"fake import library"
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	action-log //:windows_plugin:link-shared:0 \
	> "$tmp/windows-shared-log.out" 2> "$tmp/windows-shared-log.err"
contains "$tmp/windows-shared-log.out" "output_count=2"
contains "$tmp/windows-shared-log.out" "output[0]=build/qstar/out/___windows_plugin/windows_plugin.dll"
contains "$tmp/windows-shared-log.out" "output[1]=build/qstar/out/___windows_plugin/windows_plugin.lib"
contains "$tmp/windows-shared-log.out" "/IMPLIB:build/qstar/out/___windows_plugin/windows_plugin.lib"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
	--progress off build //:windows_rsp > "$tmp/windows-rsp.out" \
	2> "$tmp/windows-rsp.err"
contains "$tmp/windows-rsp.out" "response_file id=//:windows_rsp:compile:0"
contains "$tmp/windows-rsp.out" "response_file id=//:windows_rsp:link:0"
contains "$tmp/windows-rsp.out" "style=msvc"
contains "$tmp/windows-rsp.out" "status ok"
test -x "$corpus/$build_dir/out/___windows_rsp/windows_rsp.exe" ||
	fail "fake Windows executable artifact missing"
compile_rsp="$corpus/$build_dir/rsp/___windows_rsp_compile_0.rsp"
link_rsp="$corpus/$build_dir/rsp/___windows_rsp_link_0.rsp"
test -f "$compile_rsp" || fail "fake Windows compile response file missing"
test -f "$link_rsp" || fail "fake Windows link response file missing"
contains "$compile_rsp" '"/DNAME=alpha beta"'
contains "$compile_rsp" '"/DQUOTE=\"value\""'
contains "$compile_rsp" '"/DTRAIL=tail\\"'
contains "$compile_rsp" "/DSEMICOLON=a;b"
contains "$compile_rsp" '"/DWINPATH=C:\Program Files\QStar\Include"'
contains "$compile_rsp" '"/DJSON={\"path\":\"C:\qstar\include\"}"'
contains "$compile_rsp" '"/DSPACE_TRAIL=value with trailing space "'
contains "$compile_rsp" '"/DSLASHQUOTE=C:\qstar\\\"quoted\""'
contains "$link_rsp" '"/PDB:build/qstar/pdb/windows rsp.pdb"'
contains "$link_rsp" '"/MANIFESTDEPENDENCY:type='"'"'win32'"'"' name='"'"'QStar Probe'"'"'"'
contains "$link_rsp" '"/LIBPATH:sdk/lib with space/um/x64"'
contains "$link_rsp" "kernel32.lib"
contains "$link_rsp" "uuid.lib"

if command -v ninja >/dev/null 2>&1; then
	rm -rf "$corpus/$build_dir" "$corpus/.ninja_log" "$corpus/.ninja_deps"
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows -G ninja \
		--progress off build //:windows_rsp > "$tmp/windows-rsp-ninja.out" \
		2> "$tmp/windows-rsp-ninja.err"
	contains "$tmp/windows-rsp-ninja.out" "backend ninja"
	contains "$tmp/windows-rsp-ninja.out" "status ok"
	test ! -f "$corpus/.ninja_log" || fail "ninja root .ninja_log pollution"
	test ! -f "$corpus/.ninja_deps" || fail "ninja root .ninja_deps pollution"
	compile_rsp="$corpus/$build_dir/rsp/___windows_rsp_compile_0.rsp"
	link_rsp="$corpus/$build_dir/rsp/___windows_rsp_link_0.rsp"
	test -f "$compile_rsp" || fail "ninja fake Windows compile response file missing"
	test -f "$link_rsp" || fail "ninja fake Windows link response file missing"
	contains "$compile_rsp" '"/DQUOTE=\"value\""'
	contains "$compile_rsp" '"/DTRAIL=tail\\"'
	contains "$compile_rsp" '"/DWINPATH=C:\Program Files\QStar\Include"'
	contains "$compile_rsp" '"/DJSON={\"path\":\"C:\qstar\include\"}"'
	contains "$compile_rsp" '"/DSLASHQUOTE=C:\qstar\\\"quoted\""'
	contains "$link_rsp" '"/LIBPATH:sdk/lib with space/um/x64"'

	rm -rf "$corpus/$build_dir" "$corpus/.ninja_log" "$corpus/.ninja_deps"
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows \
		-G ninja --progress off build //:windows_static \
		> "$tmp/windows-static-ninja.out" 2> "$tmp/windows-static-ninja.err"
	contains "$tmp/windows-static-ninja.out" "backend ninja"
	contains "$tmp/windows-static-ninja.out" "status ok"
	test -f "$corpus/$build_dir/out/___windows_static/windows_static.lib" ||
		fail "ninja fake Windows static .lib artifact missing"
	contains "$corpus/$build_dir/out/___windows_static/windows_static.lib" "fake static library"
	contains "$corpus/$build_dir/ninja/build.ninja" "build/qstar/out/___windows_static/windows_static.lib"
	contains "$corpus/$build_dir/ninja/build.ninja" "description = Linking C static library build/qstar/out/___windows_static/windows_static.lib"
	test ! -f "$corpus/.ninja_log" || fail "ninja static .lib root .ninja_log pollution"
	test ! -f "$corpus/.ninja_deps" || fail "ninja static .lib root .ninja_deps pollution"

	rm -rf "$corpus/$build_dir" "$corpus/.ninja_log" "$corpus/.ninja_deps"
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows -G ninja \
		--progress off build //:windows_plugin > "$tmp/windows-shared-ninja.out" \
		2> "$tmp/windows-shared-ninja.err"
	contains "$tmp/windows-shared-ninja.out" "backend ninja"
	contains "$tmp/windows-shared-ninja.out" "status ok"
	test -x "$corpus/$build_dir/out/___windows_plugin/windows_plugin.dll" ||
		fail "ninja fake Windows sharedlib runtime .dll missing"
	test -f "$corpus/$build_dir/out/___windows_plugin/windows_plugin.lib" ||
		fail "ninja fake Windows sharedlib import .lib missing"
	contains "$corpus/$build_dir/ninja/build.ninja" \
		"build/qstar/out/___windows_plugin/windows_plugin.dll build/qstar/out/___windows_plugin/windows_plugin.lib: qstar_link"
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows -G ninja \
		action-log //:windows_plugin:link-shared:0 \
		> "$tmp/windows-shared-ninja-log.out" \
		2> "$tmp/windows-shared-ninja-log.err"
	contains "$tmp/windows-shared-ninja-log.out" "backend=ninja"
	contains "$tmp/windows-shared-ninja-log.out" "output_count=2"
	contains "$tmp/windows-shared-ninja-log.out" "/IMPLIB:build/qstar/out/___windows_plugin/windows_plugin.lib"
fi

"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows check \
	> "$tmp/windows-artifacts-check.out" 2> "$tmp/windows-artifacts-check.err"
contains "$tmp/windows-artifacts-check.out" "status ok"
"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
	explain //:plugin > "$tmp/windows-artifacts-plugin-explain.out" \
	2> "$tmp/windows-artifacts-plugin-explain.err"
contains "$tmp/windows-artifacts-plugin-explain.out" "artifact id=runtime role=sharedlib path=build/qstar/out/___plugin/plugin.dll install_dir=bin primary=true installable=true"
contains "$tmp/windows-artifacts-plugin-explain.out" "artifact id=import_lib role=import_lib path=build/qstar/out/___plugin/plugin.lib install_dir=lib primary=false installable=true"
contains "$tmp/windows-artifacts-plugin-explain.out" "/IMPLIB:build/qstar/out/___plugin/plugin.lib"
not_contains "$tmp/windows-artifacts-plugin-explain.out" "plan_diagnostic kind=windows-sharedlib-lowering"
"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
	dry-run //:plugin > "$tmp/windows-artifacts-plugin-dry.out" \
	2> "$tmp/windows-artifacts-plugin-dry.err"
contains "$tmp/windows-artifacts-plugin-dry.out" "artifact id=runtime role=sharedlib path=build/qstar/out/___plugin/plugin.dll install_dir=bin primary=true installable=true"
contains "$tmp/windows-artifacts-plugin-dry.out" "artifact id=import_lib role=import_lib path=build/qstar/out/___plugin/plugin.lib install_dir=lib primary=false installable=true"
contains "$tmp/windows-artifacts-plugin-dry.out" "/IMPLIB:build/qstar/out/___plugin/plugin.lib"
not_contains "$tmp/windows-artifacts-plugin-dry.out" "plan_diagnostic kind=windows-sharedlib-lowering"
not_contains "$tmp/windows-artifacts-plugin-dry.out" "-Wl,-soname"
"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
	list-targets --format json > "$tmp/windows-artifacts-list.json" \
	2> "$tmp/windows-artifacts-list.err"
contains "$tmp/windows-artifacts-list.json" "\"id\":\"runtime\",\"role\":\"sharedlib\",\"path\":\"build/qstar/out/___plugin/plugin.dll\",\"install_dir\":\"bin\",\"primary\":true,\"installable\":true"
contains "$tmp/windows-artifacts-list.json" "\"id\":\"import_lib\",\"role\":\"import_lib\",\"path\":\"build/qstar/out/___plugin/plugin.lib\",\"install_dir\":\"lib\",\"primary\":false,\"installable\":true"
"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
	stage //:plugin_layout --dry-run > "$tmp/windows-artifacts-plugin-stage.out" \
	2> "$tmp/windows-artifacts-plugin-stage.err"
contains "$tmp/windows-artifacts-plugin-stage.out" "stage_file src=build/qstar/out/___plugin/plugin.dll"
contains "$tmp/windows-artifacts-plugin-stage.out" "artifact=runtime"
contains "$tmp/windows-artifacts-plugin-stage.out" "stage_file src=build/qstar/out/___plugin/plugin.lib"
contains "$tmp/windows-artifacts-plugin-stage.out" "artifact=import_lib"
stage_manifest="$artifact_corpus/$build_dir/stage/___plugin_layout/manifest.json"
contains "$stage_manifest" "\"src\":\"build/qstar/out/___plugin/plugin.dll\""
contains "$stage_manifest" "\"artifact\":\"runtime\""
contains "$stage_manifest" "\"src\":\"build/qstar/out/___plugin/plugin.lib\""
contains "$stage_manifest" "\"artifact\":\"import_lib\""
not_contains "$stage_manifest" "\\"
"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
	dry-run //:all > "$tmp/windows-artifacts-dry.out" \
	2> "$tmp/windows-artifacts-dry.err"
contains "$tmp/windows-artifacts-dry.out" "output=build/qstar/out/___tool/tool.exe"
contains "$tmp/windows-artifacts-dry.out" "output=build/qstar/out/___core/core.lib"
contains "$tmp/windows-artifacts-dry.out" "output=build/qstar/out/___named_tool/named_tool.exe"
contains "$tmp/windows-artifacts-dry.out" "output=build/qstar/out/___named_core/named_core.lib"
contains "$tmp/windows-artifacts-dry.out" "response_style=msvc"
contains "$tmp/windows-artifacts-dry.out" '"/DQSTAR_WINDOWS_ARTIFACT=alpha beta"'
contains "$tmp/windows-artifacts-dry.out" \
	'"/DQSTAR_WINDOWS_PATH=C:\\Program Files\\QStar\\Include"'
contains "$tmp/windows-artifacts-dry.out" '"/DQSTAR_WINDOWS_QUOTE=\"artifact\""'

"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
	--progress off build //:all > "$tmp/windows-artifacts-build.out" \
	2> "$tmp/windows-artifacts-build.err"
contains "$tmp/windows-artifacts-build.out" "status ok"
test -x "$artifact_corpus/$build_dir/out/___tool/tool.exe" ||
	fail "Windows artifact corpus tool.exe missing"
test -x "$artifact_corpus/$build_dir/out/___named_tool/named_tool.exe" ||
	fail "Windows artifact corpus named_tool.exe missing"
test -f "$artifact_corpus/$build_dir/out/___core/core.lib" ||
	fail "Windows artifact corpus core.lib missing"
test -f "$artifact_corpus/$build_dir/out/___named_core/named_core.lib" ||
	fail "Windows artifact corpus named_core.lib missing"
test -x "$artifact_corpus/$build_dir/out/___plugin/plugin.dll" ||
	fail "Windows artifact corpus plugin.dll missing"
test -f "$artifact_corpus/$build_dir/out/___plugin/plugin.lib" ||
	fail "Windows artifact corpus plugin.lib missing"
test -x "$artifact_corpus/$build_dir/out/___plugin_user/plugin_user.exe" ||
	fail "Windows artifact corpus plugin_user.exe missing"
contains "$artifact_corpus/$build_dir/out/___core/core.lib" "fake static library"
contains "$artifact_corpus/$build_dir/out/___named_core/named_core.lib" \
	"fake static library"
contains "$artifact_corpus/$build_dir/out/___plugin/plugin.lib" \
	"fake import library"
contains "$artifact_corpus/$build_dir/out/___plugin/plugin.lib" \
	"runtime=build/qstar/out/___plugin/plugin.dll"
"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
	action-log //:plugin:link-shared:0 > "$tmp/windows-artifacts-plugin-log.out" \
	2> "$tmp/windows-artifacts-plugin-log.err"
contains "$tmp/windows-artifacts-plugin-log.out" "output_count=2"
contains "$tmp/windows-artifacts-plugin-log.out" "output[0]=build/qstar/out/___plugin/plugin.dll"
contains "$tmp/windows-artifacts-plugin-log.out" "output[1]=build/qstar/out/___plugin/plugin.lib"
contains "$tmp/windows-artifacts-plugin-log.out" "/IMPLIB:build/qstar/out/___plugin/plugin.lib"
"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
	action-log //:plugin_user:link:0 > "$tmp/windows-artifacts-plugin-user-log.out" \
	2> "$tmp/windows-artifacts-plugin-user-log.err"
contains "$tmp/windows-artifacts-plugin-user-log.out" "build/qstar/out/___plugin/plugin.lib"
not_contains "$tmp/windows-artifacts-plugin-user-log.out" "build/qstar/out/___plugin/plugin.dll"

"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
	stage //:layout > "$tmp/windows-artifacts-stage.out" \
	2> "$tmp/windows-artifacts-stage.err"
contains "$tmp/windows-artifacts-stage.out" "stage_file src=build/qstar/out/___tool/tool.exe"
contains "$tmp/windows-artifacts-stage.out" "stage_file src=build/qstar/out/___core/core.lib"
test -f "$artifact_corpus/$build_dir/stage/windows-layout/bin/tool.exe" ||
	fail "Windows artifact corpus staged tool.exe missing"
test -f "$artifact_corpus/$build_dir/stage/windows-layout/bin/named_tool.exe" ||
	fail "Windows artifact corpus staged named_tool.exe missing"
test -f "$artifact_corpus/$build_dir/stage/windows-layout/lib/core.lib" ||
	fail "Windows artifact corpus staged core.lib missing"
test -f "$artifact_corpus/$build_dir/stage/windows-layout/lib/named_core.lib" ||
	fail "Windows artifact corpus staged named_core.lib missing"
stage_manifest="$artifact_corpus/$build_dir/stage/___layout/manifest.json"
contains "$stage_manifest" "\"schema\":\"qstar-stage-manifest-v2\""
contains "$stage_manifest" "\"root\":\"build/qstar/stage/windows-layout\""
contains "$stage_manifest" "\"dst\":\"build/qstar/stage/windows-layout/bin/tool.exe\""
contains "$stage_manifest" "\"dst\":\"build/qstar/stage/windows-layout/lib/core.lib\""
contains "$stage_manifest" "\"kind\":\"target\""
contains "$stage_manifest" "\"producer\":\"//:tool\""
not_contains "$stage_manifest" "\\"

"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
	stage //:plugin_layout > "$tmp/windows-artifacts-plugin-stage-real.out" \
	2> "$tmp/windows-artifacts-plugin-stage-real.err"
contains "$tmp/windows-artifacts-plugin-stage-real.out" "status ok"
test -f "$artifact_corpus/$build_dir/stage/windows-plugin/bin/plugin.dll" ||
	fail "Windows artifact corpus staged plugin.dll missing"
test -f "$artifact_corpus/$build_dir/stage/windows-plugin/lib/plugin.lib" ||
	fail "Windows artifact corpus staged plugin.lib missing"

if command -v ninja >/dev/null 2>&1; then
	rm -rf "$artifact_corpus/$build_dir" "$artifact_corpus/.ninja_log" \
		"$artifact_corpus/.ninja_deps"
	"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
		-G ninja --progress off build //:all \
		> "$tmp/windows-artifacts-ninja-build.out" \
		2> "$tmp/windows-artifacts-ninja-build.err"
	contains "$tmp/windows-artifacts-ninja-build.out" "backend ninja"
	contains "$tmp/windows-artifacts-ninja-build.out" "status ok"
		test -x "$artifact_corpus/$build_dir/out/___tool/tool.exe" ||
			fail "Ninja Windows artifact corpus tool.exe missing"
		test -x "$artifact_corpus/$build_dir/out/___named_tool/named_tool.exe" ||
			fail "Ninja Windows artifact corpus named_tool.exe missing"
		test -f "$artifact_corpus/$build_dir/out/___core/core.lib" ||
			fail "Ninja Windows artifact corpus core.lib missing"
		test -f "$artifact_corpus/$build_dir/out/___named_core/named_core.lib" ||
			fail "Ninja Windows artifact corpus named_core.lib missing"
		test -x "$artifact_corpus/$build_dir/out/___plugin/plugin.dll" ||
			fail "Ninja Windows artifact corpus plugin.dll missing"
		test -f "$artifact_corpus/$build_dir/out/___plugin/plugin.lib" ||
			fail "Ninja Windows artifact corpus plugin.lib missing"
		test -x "$artifact_corpus/$build_dir/out/___plugin_user/plugin_user.exe" ||
			fail "Ninja Windows artifact corpus plugin_user.exe missing"
		contains "$artifact_corpus/$build_dir/ninja/build.ninja" \
			"build/qstar/out/___tool/tool.exe"
		contains "$artifact_corpus/$build_dir/ninja/build.ninja" \
			"build/qstar/out/___core/core.lib"
		contains "$artifact_corpus/$build_dir/ninja/build.ninja" \
			"build/qstar/out/___plugin/plugin.dll build/qstar/out/___plugin/plugin.lib: qstar_link"
		contains "$artifact_corpus/$build_dir/ninja/build.ninja" \
			"build/qstar/out/___plugin_user/plugin_user.exe: qstar_link build/qstar/out/___plugin_user/obj0.o build/qstar/out/___plugin/plugin.lib"
		"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
			-G ninja action-log //:plugin:link-shared:0 \
			> "$tmp/windows-artifacts-ninja-plugin-log.out" \
			2> "$tmp/windows-artifacts-ninja-plugin-log.err"
		contains "$tmp/windows-artifacts-ninja-plugin-log.out" "backend=ninja"
		contains "$tmp/windows-artifacts-ninja-plugin-log.out" "output_count=2"
		contains "$tmp/windows-artifacts-ninja-plugin-log.out" "/IMPLIB:build/qstar/out/___plugin/plugin.lib"
		"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
			-G ninja action-log //:plugin_user:link:0 \
			> "$tmp/windows-artifacts-ninja-plugin-user-log.out" \
			2> "$tmp/windows-artifacts-ninja-plugin-user-log.err"
		contains "$tmp/windows-artifacts-ninja-plugin-user-log.out" "backend=ninja"
		contains "$tmp/windows-artifacts-ninja-plugin-user-log.out" "build/qstar/out/___plugin/plugin.lib"
		not_contains "$tmp/windows-artifacts-ninja-plugin-user-log.out" "build/qstar/out/___plugin/plugin.dll"
		test ! -f "$artifact_corpus/.ninja_log" ||
			fail "Windows artifact corpus root .ninja_log pollution"
		test ! -f "$artifact_corpus/.ninja_deps" ||
			fail "Windows artifact corpus root .ninja_deps pollution"

	"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
		-G ninja stage //:layout > "$tmp/windows-artifacts-ninja-stage.out" \
		2> "$tmp/windows-artifacts-ninja-stage.err"
	contains "$tmp/windows-artifacts-ninja-stage.out" "backend ninja"
	test -f "$artifact_corpus/$build_dir/stage/windows-layout/bin/tool.exe" ||
		fail "Ninja Windows artifact corpus staged tool.exe missing"
	test -f "$artifact_corpus/$build_dir/stage/windows-layout/lib/core.lib" ||
		fail "Ninja Windows artifact corpus staged core.lib missing"
	stage_manifest="$artifact_corpus/$build_dir/stage/___layout/manifest.json"
	contains "$stage_manifest" "\"dst\":\"build/qstar/stage/windows-layout/bin/tool.exe\""
	contains "$stage_manifest" "\"dst\":\"build/qstar/stage/windows-layout/lib/core.lib\""
	not_contains "$stage_manifest" "\\"

		"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows \
			-G ninja stage //:plugin_layout > "$tmp/windows-artifacts-ninja-plugin-stage.out" \
			2> "$tmp/windows-artifacts-ninja-plugin-stage.err"
		contains "$tmp/windows-artifacts-ninja-plugin-stage.out" "backend ninja"
		test -f "$artifact_corpus/$build_dir/stage/windows-plugin/bin/plugin.dll" ||
			fail "Ninja Windows artifact corpus staged plugin.dll missing"
		test -f "$artifact_corpus/$build_dir/stage/windows-plugin/lib/plugin.lib" ||
			fail "Ninja Windows artifact corpus staged plugin.lib missing"
	fi

mkdir -p "$tmp/bad-artifact-selector/src"
cat > "$tmp/bad-artifact-selector/src/plugin.c" <<'EOF'
int plugin(void) { return 0; }
EOF
cat > "$tmp/bad-artifact-selector/qstar.lua" <<'EOF'
qstar.sharedlib "plugin" {
  sources = {"src/plugin.c"},
}

qstar.stage "bad" {
  root = "stage/bad",
  files = {
    qstar.stage_file(qstar.target_file("//:plugin", { artifact = "pdb" }), "lib/plugin.pdb"),
  },
}
EOF
if "$qstar" --file "$tmp/bad-artifact-selector/qstar.lua" --qstar-internal-platform windows \
	check //:bad > "$tmp/bad-artifact-selector.out" \
	2> "$tmp/bad-artifact-selector.err"; then
	fail "unknown target_file artifact selector unexpectedly succeeded"
fi
contains "$tmp/bad-artifact-selector.err" "target_file artifact selector 'pdb' is unknown for target '//:plugin'"
contains "$tmp/bad-artifact-selector.err" "known artifacts: runtime, import_lib"

mkdir -p "$tmp/bad-drive" "$tmp/bad-drive-slash" "$tmp/bad-backslash/src"
cat > "$tmp/bad-drive/qstar.lua" <<'EOF'
qstar.executable "bad" {
  sources = {"C:\\project\\src\\main.c"},
}
EOF
if "$qstar" --file "$tmp/bad-drive/qstar.lua" check //:bad \
	> "$tmp/bad-drive.out" 2> "$tmp/bad-drive.err"; then
	fail "drive-letter source path unexpectedly succeeded"
fi
contains "$tmp/bad-drive.err" "must be package-relative"
contains "$tmp/bad-drive.err" "drive-letter paths are not allowed"
contains "$tmp/bad-drive.err" "write project files as slash-normalized paths"

cat > "$tmp/bad-drive-slash/qstar.lua" <<'EOF'
qstar.executable "bad" {
  sources = {"C:/project/src/main.c"},
}
EOF
if "$qstar" --file "$tmp/bad-drive-slash/qstar.lua" check //:bad \
	> "$tmp/bad-drive-slash.out" 2> "$tmp/bad-drive-slash.err"; then
	fail "slash drive-letter source path unexpectedly succeeded"
fi
contains "$tmp/bad-drive-slash.err" "must be package-relative"
contains "$tmp/bad-drive-slash.err" "drive-letter paths are not allowed"
contains "$tmp/bad-drive-slash.err" "absolute tool locations in toolsets"

cat > "$tmp/bad-backslash/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/bad-backslash/qstar.lua" <<'EOF'
qstar.executable "bad" {
  sources = {"src/main.c"},
  lang = {
    c = {
      include_dirs = {"sdk\\include"},
    },
  },
}
EOF
if "$qstar" --file "$tmp/bad-backslash/qstar.lua" check //:bad \
	> "$tmp/bad-backslash.out" 2> "$tmp/bad-backslash.err"; then
	fail "backslash include path unexpectedly succeeded"
fi
contains "$tmp/bad-backslash.err" "must be package-relative"
contains "$tmp/bad-backslash.err" "backslash paths are not normalized"
contains "$tmp/bad-backslash.err" "use '/' separators"

mkdir -p "$tmp/bad-stage-root/src" "$tmp/bad-stage-dst/src"
cat > "$tmp/bad-stage-root/src/payload.txt" <<'EOF'
payload
EOF
cat > "$tmp/bad-stage-root/qstar.lua" <<'EOF'
qstar.stage "bad" {
  root = "stage\\bad",
  files = {
    qstar.stage_file("src/payload.txt", "share/payload.txt"),
  },
}
EOF
if "$qstar" --file "$tmp/bad-stage-root/qstar.lua" check //:bad \
	> "$tmp/bad-stage-root.out" 2> "$tmp/bad-stage-root.err"; then
	fail "backslash stage root unexpectedly succeeded"
fi
contains "$tmp/bad-stage-root.err" "stage root"
contains "$tmp/bad-stage-root.err" "backslash paths are not normalized"
contains "$tmp/bad-stage-root.err" "use '/' separators"

cat > "$tmp/bad-stage-dst/src/payload.txt" <<'EOF'
payload
EOF
cat > "$tmp/bad-stage-dst/qstar.lua" <<'EOF'
qstar.stage "bad" {
  root = "stage/bad",
  files = {
    qstar.stage_file("src/payload.txt", "share\\payload.txt"),
  },
}
EOF
if "$qstar" --file "$tmp/bad-stage-dst/qstar.lua" check //:bad \
	> "$tmp/bad-stage-dst.out" 2> "$tmp/bad-stage-dst.err"; then
	fail "backslash stage destination unexpectedly succeeded"
fi
contains "$tmp/bad-stage-dst.err" "stage destination"
contains "$tmp/bad-stage-dst.err" "backslash paths are not normalized"
contains "$tmp/bad-stage-dst.err" "use '/' separators"

printf 'qstar-windows-prep: passed\n'
