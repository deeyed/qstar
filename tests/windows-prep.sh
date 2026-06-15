#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-windows-prep.$$
corpus=tests/corpus/response-files
artifact_corpus=tests/corpus/windows-artifacts
build_dir=build/qstar

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

rm -rf "$tmp"
mkdir -p "$tmp"
rm -rf "$corpus/build" "$corpus/stage" "$artifact_corpus/build" "$artifact_corpus/stage"
rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps"
rm -f "$artifact_corpus/.ninja_log" "$artifact_corpus/.ninja_deps"
trap 'rm -rf "$tmp"; rm -rf "$corpus/build" "$corpus/stage" "$artifact_corpus/build" "$artifact_corpus/stage"; rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps" "$artifact_corpus/.ninja_log" "$artifact_corpus/.ninja_deps"' EXIT HUP INT TERM

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

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang dry-run \
	//:windows_app > "$tmp/windows-dry.out" 2> "$tmp/windows-dry.err"
contains "$tmp/windows-dry.out" "response_style=msvc"
contains "$tmp/windows-dry.out" "response=skeleton"
contains "$tmp/windows-dry.out" "response_file=build/qstar/rsp/"
contains "$tmp/windows-dry.out" "/link"
contains "$tmp/windows-dry.out" "/LIBPATH:sdk/lib/um/x64"
contains "$tmp/windows-dry.out" "kernel32.lib"
contains "$tmp/windows-dry.out" "output=build/qstar/out/___windows_app/windows_app.exe"
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang explain \
	//:windows_app > "$tmp/windows-explain.out" 2> "$tmp/windows-explain.err"
contains "$tmp/windows-explain.out" "Linking C executable build/qstar/out/___windows_app/windows_app.exe"
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang list-targets \
	--format json > "$tmp/windows-list.json" 2> "$tmp/windows-list.err"
contains "$tmp/windows-list.json" "\"artifact_name\":\"windows_app.exe\""

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang dry-run \
	//:windows_mapped > "$tmp/windows-mapped-dry.out" 2> "$tmp/windows-mapped-dry.err"
contains "$tmp/windows-mapped-dry.out" "response_style=msvc"
contains "$tmp/windows-mapped-dry.out" "output=build/qstar/out/___windows_mapped/mapped_named.exe"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang dry-run \
	//:windows_static > "$tmp/windows-static-dry.out" 2> "$tmp/windows-static-dry.err"
contains "$tmp/windows-static-dry.out" "final_action=archive"
contains "$tmp/windows-static-dry.out" "output=build/qstar/out/___windows_static/windows_static.lib"
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
	--progress off build //:windows_static > "$tmp/windows-static-build.out" \
	2> "$tmp/windows-static-build.err"
contains "$tmp/windows-static-build.out" "response_file id=//:windows_static:compile:0"
contains "$tmp/windows-static-build.out" "status ok"
test -f "$corpus/$build_dir/out/___windows_static/windows_static.lib" ||
	fail "fake Windows static .lib artifact missing"
contains "$corpus/$build_dir/out/___windows_static/windows_static.lib" "fake static library"
"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
	action-log //:windows_static:archive:0 > "$tmp/windows-static-log.out" \
	2> "$tmp/windows-static-log.err"
contains "$tmp/windows-static-log.out" "argv[0]=tools/fake-lib"
contains "$tmp/windows-static-log.out" "windows_static.lib"
contains "$tmp/windows-static-log.out" "description='Linking C static library build/qstar/out/___windows_static/windows_static.lib'"

if "$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
	build //:windows_plugin > "$tmp/windows-shared.out" \
	2> "$tmp/windows-shared.err"; then
	fail "Windows sharedlib unexpectedly succeeded"
fi
contains "$tmp/windows-shared.err" "Windows shared libraries require a runtime .dll"
contains "$tmp/windows-shared.err" "import .lib"
contains "$tmp/windows-shared.err" "docs/windows-artifact-policy.md"

"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
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
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang -G ninja \
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
	"$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
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

	if "$qstar" --file "$corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang -G ninja \
		build //:windows_plugin > "$tmp/windows-shared-ninja.out" \
		2> "$tmp/windows-shared-ninja.err"; then
		fail "Windows sharedlib Ninja unexpectedly succeeded"
	fi
	contains "$tmp/windows-shared-ninja.err" "Windows shared libraries require a runtime .dll"
	contains "$tmp/windows-shared-ninja.err" "import .lib"
	contains "$tmp/windows-shared-ninja.err" "docs/windows-artifact-policy.md"
fi

"$qstar" --file "$artifact_corpus/qstar.lua" check \
	> "$tmp/windows-artifacts-check.out" 2> "$tmp/windows-artifacts-check.err"
contains "$tmp/windows-artifacts-check.out" "status ok"
"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
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

"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
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
contains "$artifact_corpus/$build_dir/out/___core/core.lib" "fake static library"
contains "$artifact_corpus/$build_dir/out/___named_core/named_core.lib" \
	"fake static library"

"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
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

"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
	install //:tool --prefix "$tmp/windows-artifacts-prefix" \
	> "$tmp/windows-artifacts-install-tool.out" \
	2> "$tmp/windows-artifacts-install-tool.err"
test -f "$tmp/windows-artifacts-prefix/bin/tool.exe" ||
	fail "Windows artifact corpus installed tool.exe missing"
"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
	install //:core --prefix "$tmp/windows-artifacts-prefix" \
	> "$tmp/windows-artifacts-install-core.out" \
	2> "$tmp/windows-artifacts-install-core.err"
test -f "$tmp/windows-artifacts-prefix/lib/core.lib" ||
	fail "Windows artifact corpus installed core.lib missing"
"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
	install //:named_tool --prefix "$tmp/windows-artifacts-prefix" \
	> "$tmp/windows-artifacts-install-named-tool.out" \
	2> "$tmp/windows-artifacts-install-named-tool.err"
test -f "$tmp/windows-artifacts-prefix/bin/named_tool.exe" ||
	fail "Windows artifact corpus installed named_tool.exe missing"
"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
	install //:named_core --prefix "$tmp/windows-artifacts-prefix" \
	> "$tmp/windows-artifacts-install-named-core.out" \
	2> "$tmp/windows-artifacts-install-named-core.err"
test -f "$tmp/windows-artifacts-prefix/lib/named_core.lib" ||
	fail "Windows artifact corpus installed named_core.lib missing"

if "$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
	build //:plugin > "$tmp/windows-artifacts-shared.out" \
	2> "$tmp/windows-artifacts-shared.err"; then
	fail "Windows artifact corpus sharedlib unexpectedly succeeded"
fi
contains "$tmp/windows-artifacts-shared.err" "Windows shared libraries require a runtime .dll"
contains "$tmp/windows-artifacts-shared.err" "import .lib"
contains "$tmp/windows-artifacts-shared.err" "docs/windows-artifact-policy.md"

if command -v ninja >/dev/null 2>&1; then
	rm -rf "$artifact_corpus/$build_dir" "$artifact_corpus/.ninja_log" \
		"$artifact_corpus/.ninja_deps"
	"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
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
	contains "$artifact_corpus/$build_dir/ninja/build.ninja" \
		"build/qstar/out/___tool/tool.exe"
	contains "$artifact_corpus/$build_dir/ninja/build.ninja" \
		"build/qstar/out/___core/core.lib"
	test ! -f "$artifact_corpus/.ninja_log" ||
		fail "Windows artifact corpus root .ninja_log pollution"
	test ! -f "$artifact_corpus/.ninja_deps" ||
		fail "Windows artifact corpus root .ninja_deps pollution"

	"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
		-G ninja stage //:layout > "$tmp/windows-artifacts-ninja-stage.out" \
		2> "$tmp/windows-artifacts-ninja-stage.err"
	contains "$tmp/windows-artifacts-ninja-stage.out" "backend ninja"
	test -f "$artifact_corpus/$build_dir/stage/windows-layout/bin/tool.exe" ||
		fail "Ninja Windows artifact corpus staged tool.exe missing"
	test -f "$artifact_corpus/$build_dir/stage/windows-layout/lib/core.lib" ||
		fail "Ninja Windows artifact corpus staged core.lib missing"

	"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
		-G ninja install //:tool --prefix "$tmp/windows-artifacts-ninja-prefix" \
		> "$tmp/windows-artifacts-ninja-install-tool.out" \
		2> "$tmp/windows-artifacts-ninja-install-tool.err"
	test -f "$tmp/windows-artifacts-ninja-prefix/bin/tool.exe" ||
		fail "Ninja Windows artifact corpus installed tool.exe missing"
	"$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
		-G ninja install //:core --prefix "$tmp/windows-artifacts-ninja-prefix" \
		> "$tmp/windows-artifacts-ninja-install-core.out" \
		2> "$tmp/windows-artifacts-ninja-install-core.err"
	test -f "$tmp/windows-artifacts-ninja-prefix/lib/core.lib" ||
		fail "Ninja Windows artifact corpus installed core.lib missing"

	if "$qstar" --file "$artifact_corpus/qstar.lua" --qstar-internal-platform windows --qstar-internal-toolchain clang \
		-G ninja build //:plugin > "$tmp/windows-artifacts-shared-ninja.out" \
		2> "$tmp/windows-artifacts-shared-ninja.err"; then
		fail "Windows artifact corpus Ninja sharedlib unexpectedly succeeded"
	fi
	contains "$tmp/windows-artifacts-shared-ninja.err" \
		"Windows shared libraries require a runtime .dll"
	contains "$tmp/windows-artifacts-shared-ninja.err" "import .lib"
	contains "$tmp/windows-artifacts-shared-ninja.err" \
		"docs/windows-artifact-policy.md"
fi

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

printf 'qstar-windows-prep: passed\n'
