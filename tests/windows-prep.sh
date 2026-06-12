#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-windows-prep.$$
corpus=tests/corpus/response-files
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
rm -rf "$corpus/build" "$corpus/stage"
rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps"
trap 'rm -rf "$tmp"; rm -rf "$corpus/build" "$corpus/stage"; rm -f "$corpus/.ninja_log" "$corpus/.ninja_deps"' EXIT HUP INT TERM

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

"$qstar" --file "$corpus/qstar.lua" --profile windows-msvc dry-run \
	//:windows_app > "$tmp/windows-dry.out" 2> "$tmp/windows-dry.err"
contains "$tmp/windows-dry.out" "response_style=msvc"
contains "$tmp/windows-dry.out" "response=skeleton"
contains "$tmp/windows-dry.out" "response_file=build/qstar/rsp/"
contains "$tmp/windows-dry.out" "/link"
contains "$tmp/windows-dry.out" "/LIBPATH:sdk/lib/um/x64"
contains "$tmp/windows-dry.out" "kernel32.lib"
contains "$tmp/windows-dry.out" "output=build/qstar/out/___windows_app/windows_app.exe"

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

printf 'qstar-windows-prep: passed\n'
