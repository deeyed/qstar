#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-smoke.$$

fail() {
	echo "qstar-smoke: $*" >&2
	exit 1
}

contains() {
	file=$1
	pat=$2
	grep -F -q -- "$pat" "$file" || fail "missing pattern '$pat' in $file"
}

rm -rf "$tmp"
mkdir -p "$tmp/src"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cat > "$tmp/qstar.lua" <<'EOF'
qstar.exe "app" {
  sources = {"src/main.c"},
}
EOF

cat > "$tmp/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF

"$qstar" --file "$tmp/qstar.lua" build //:app > "$tmp/first.out" 2> "$tmp/first.err"
contains "$tmp/first.out" "qstar build v2"
contains "$tmp/first.out" "status=run"
contains "$tmp/first.out" "status ok"
test -f "$tmp/.qstar/state/actions.json" || fail "missing action state"
test -f "$tmp/compile_commands.json" || fail "missing compile_commands.json"
contains "$tmp/compile_commands.json" "src/main.c"

"$qstar" --file "$tmp/qstar.lua" build //:app > "$tmp/second.out" 2> "$tmp/second.err"
contains "$tmp/second.out" "status=skip"

"$qstar" --file "$tmp/qstar.lua" why-rebuild //:app > "$tmp/why.out" 2> "$tmp/why.err"
contains "$tmp/why.out" "qstar why-rebuild v1"
contains "$tmp/why.out" "status=skip"

cat > "$tmp/src/main.c" <<'EOF'
int main(void) { return 1 - 1; }
EOF

"$qstar" --file "$tmp/qstar.lua" build //:app --explain-cache > "$tmp/third.out" 2> "$tmp/third.err"
contains "$tmp/third.out" "cache_miss id=//:app:compile:0"
contains "$tmp/third.out" "status=run"

"$qstar" --file "$tmp/qstar.lua" log //:app > "$tmp/log.out" 2> "$tmp/log.err"
contains "$tmp/log.out" "qstar log v1"
contains "$tmp/log.out" "log_file .qstar/logs/___app_compile_0.log"

cat > "$tmp/src/main.c" <<'EOF'
int main(void) { return ; }
EOF

if "$qstar" --file "$tmp/qstar.lua" --diagnostics json build //:app > "$tmp/fail.out" 2> "$tmp/fail.err"; then
	fail "invalid C build unexpectedly succeeded"
fi
contains "$tmp/fail.err" "\"schema\":\"qstar-diagnostic-v1\""
contains "$tmp/fail.err" "\"field\":\"action\""
test -f "$tmp/.qstar/logs/last-failure.replay" || fail "missing failure replay"

"$qstar" --file "$tmp/qstar.lua" last-failure > "$tmp/replay.out" 2> "$tmp/replay.err"
contains "$tmp/replay.out" "qstar last-failure v1"
contains "$tmp/replay.out" "cc -c src/main.c"

"$qstar" --file "$tmp/qstar.lua" clean --target //:app > "$tmp/clean-target.out" 2> "$tmp/clean-target.err"
contains "$tmp/clean-target.out" "qstar clean v1"
test ! -d "$tmp/.qstar/out/___app" || fail "target clean left target output"

"$qstar" --file "$tmp/qstar.lua" clean > "$tmp/clean.out" 2> "$tmp/clean.err"
contains "$tmp/clean.out" "clean_all .qstar compile_commands.json"
test ! -d "$tmp/.qstar" || fail "clean left .qstar"
test ! -f "$tmp/compile_commands.json" || fail "clean left compile_commands.json"

mkdir -p "$tmp/tools"
cat > "$tmp/tools/gen-value.sh" <<'EOF'
#!/bin/sh
set -eu
out=$1
mkdir -p "$(dirname "$out")"
cat > "$out" <<'SRC'
int generated_value(void) { return 41; }
SRC
EOF
chmod +x "$tmp/tools/gen-value.sh"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.config_header "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"APP_VALUE=41", "APP_FEATURE"},
}

qstar.genrule "make_value" {
  tool = "tools/gen-value.sh",
  outputs = {qstar.output("generated/value.c")},
  args = {"generated/value.c"},
}

qstar.exe "genapp" {
  sources = {"src/main.c", qstar.output("generated/value.c")},
  private_headers = {qstar.output("generated/config.h")},
  include_dirs = {"generated"},
}
EOF

cat > "$tmp/src/main.c" <<'EOF'
#include "config.h"
int generated_value(void);
int main(void) { return generated_value() - APP_VALUE; }
EOF

"$qstar" --file "$tmp/qstar.lua" build //:genapp > "$tmp/generated-first.out" 2> "$tmp/generated-first.err"
contains "$tmp/generated-first.out" "build_action id=//:cfg:generate:0 status=run"
contains "$tmp/generated-first.out" "build_action id=//:make_value:generate:0 status=run"
contains "$tmp/generated-first.out" "status ok"
test -f "$tmp/generated/config.h" || fail "missing generated config header"
test -f "$tmp/generated/value.c" || fail "missing generated source"
contains "$tmp/generated/config.h" "#define APP_VALUE 41"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.config_header "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"APP_VALUE=42", "APP_FEATURE"},
}

qstar.genrule "make_value" {
  tool = "tools/gen-value.sh",
  outputs = {qstar.output("generated/value.c")},
  args = {"generated/value.c"},
}

qstar.exe "genapp" {
  sources = {"src/main.c", qstar.output("generated/value.c")},
  private_headers = {qstar.output("generated/config.h")},
  include_dirs = {"generated"},
}
EOF

"$qstar" --file "$tmp/qstar.lua" build //:genapp --explain-cache > "$tmp/generated-second.out" 2> "$tmp/generated-second.err"
contains "$tmp/generated-second.out" "cache_miss id=//:cfg:generate:0"
contains "$tmp/generated-second.out" "cache_miss id=//:genapp:compile:0"
contains "$tmp/generated/config.h" "#define APP_VALUE 42"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.genrule "one" {
  tool = "tools/gen-value.sh",
  outputs = {qstar.output("generated/collision.c")},
  args = {"generated/collision.c"},
}

qstar.genrule "two" {
  tool = "tools/gen-value.sh",
  outputs = {qstar.output("generated/collision.c")},
  args = {"generated/collision.c"},
}
EOF

if "$qstar" --file "$tmp/qstar.lua" check > "$tmp/collision.out" 2> "$tmp/collision.err"; then
	fail "duplicate generated output unexpectedly succeeded"
fi
contains "$tmp/collision.err" "multiple producers"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.genrule "bad_out" {
  tool = "tools/gen-value.sh",
  outputs = {qstar.output("../bad.c")},
  args = {"../bad.c"},
}
EOF

if "$qstar" --file "$tmp/qstar.lua" check > "$tmp/outside.out" 2> "$tmp/outside.err"; then
	fail "outside generated output unexpectedly succeeded"
fi
contains "$tmp/outside.err" "must be package-relative"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.exe "bad_suffix" {
  sources = {"src/main.txt"},
}
EOF
cat > "$tmp/src/main.txt" <<'EOF'
not source
EOF

if "$qstar" --file "$tmp/qstar.lua" check //:bad_suffix > "$tmp/suffix.out" 2> "$tmp/suffix.err"; then
	fail "unsupported suffix unexpectedly succeeded"
fi
contains "$tmp/suffix.err" "unsupported source extension"

cat > "$tmp/tools/cale" <<'EOF'
#!/bin/sh
set -eu
mode=link
out=
src=
prev=
for arg in "$@"; do
  if [ "$prev" = "-o" ]; then
    out=$arg
    prev=
    continue
  fi
  case "$arg" in
    -c) mode=compile ;;
    -o) prev="-o" ;;
    --target=*) ;;
    -*) ;;
    *) src=$arg ;;
  esac
done
if [ "$mode" = "compile" ]; then
  case "$src" in
    *.cale)
      tmp=${TMPDIR:-/tmp}/qstar-fake-cale.$$.c
      printf '%s\n' 'int cale_unit(void) { return 7; }' > "$tmp"
      cc -c "$tmp" -o "$out"
      rm -f "$tmp"
      ;;
    *)
      cc -c "$src" -o "$out"
      ;;
  esac
else
  cc "$@"
fi
EOF
chmod +x "$tmp/tools/cale"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.exe "mixed" {
  toolchain = "cale",
  sources = {"src/main.c", "src/unit.cale"},
}

qstar.staticlib "calelib" {
  toolchain = "cale",
  sources = {"src/unit.cale"},
}
EOF
cat > "$tmp/src/main.c" <<'EOF'
int cale_unit(void);
int main(void) { return cale_unit() - 7; }
EOF
cat > "$tmp/src/unit.cale" <<'EOF'
fn cale_unit() -> int { return 7; }
EOF

PATH="$tmp/tools:$PATH" "$qstar" --file "$tmp/qstar.lua" dry-run //:mixed > "$tmp/mixed-dry.out" 2> "$tmp/mixed-dry.err"
contains "$tmp/mixed-dry.out" "argv=[cale, -c, src/main.c"
contains "$tmp/mixed-dry.out" "argv=[cale, -c, src/unit.cale"
PATH="$tmp/tools:$PATH" "$qstar" --file "$tmp/qstar.lua" build //:mixed > "$tmp/mixed-build.out" 2> "$tmp/mixed-build.err"
contains "$tmp/mixed-build.out" "status ok"
PATH="$tmp/tools:$PATH" "$qstar" --file "$tmp/qstar.lua" build //:calelib > "$tmp/cale-only.out" 2> "$tmp/cale-only.err"
contains "$tmp/cale-only.out" "status ok"
contains "$tmp/compile_commands.json" "src/unit.cale"

if PATH=/nonexistent "$qstar" --file "$tmp/qstar.lua" build //:mixed > "$tmp/no-cale.out" 2> "$tmp/no-cale.err"; then
	fail "missing cale compiler unexpectedly succeeded"
fi
contains "$tmp/no-cale.err" "Cale compiler 'cale' not found"

mkdir -p "$tmp/include" "$tmp/src/core_private" "$tmp/lib"
cat > "$tmp/include/core.h" <<'EOF'
int core_value(void);
EOF
cat > "$tmp/src/core_private/core_private.h" <<'EOF'
#define CORE_PRIVATE_VALUE 13
EOF
cat > "$tmp/src/core.c" <<'EOF'
#include "core_private.h"
int core_value(void) { return CORE_PRIVATE_VALUE; }
EOF
cat > "$tmp/src/util.c" <<'EOF'
#include "core.h"
int util_value(void) { return core_value(); }
EOF
cat > "$tmp/src/link_main.c" <<'EOF'
#include "core.h"
int util_value(void);
int main(void) { return util_value() - core_value(); }
EOF
cat > "$tmp/src/bad_private.c" <<'EOF'
#include "core_private.h"
int main(void) { return CORE_PRIVATE_VALUE; }
EOF
cat > "$tmp/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  sources = {"src/core.c"},
  public_headers = {"include/core.h"},
  public_include_dirs = {"include"},
  private_include_dirs = {"src/core_private"},
}

qstar.staticlib "util" {
  sources = {"src/util.c"},
  deps = {"//:core"},
}

qstar.exe "linkapp" {
  sources = {"src/link_main.c"},
  deps = {"//:util"},
}

qstar.exe "bad_private" {
  sources = {"src/bad_private.c"},
  deps = {"//:core"},
}

qstar.exe "sysflags" {
  sources = {"src/link_main.c"},
  libs = {"m"},
  lib_dirs = {"lib"},
}

qstar.sharedlib "plugin" {
  sources = {"src/core.c"},
}
EOF

"$qstar" --file "$tmp/qstar.lua" build //:linkapp > "$tmp/linkapp.out" 2> "$tmp/linkapp.err"
contains "$tmp/linkapp.out" "status ok"
case "$(cat "$tmp/.qstar/logs/___linkapp_link_0.log")" in
  *libutil.a*libcore.a*) ;;
  *) fail "link order did not include util before core" ;;
esac

if "$qstar" --file "$tmp/qstar.lua" build //:bad_private > "$tmp/bad-private.out" 2> "$tmp/bad-private.err"; then
	fail "private include propagation unexpectedly succeeded"
fi
contains "$tmp/bad-private.err" "action '//:bad_private:compile:0' failed"

"$qstar" --file "$tmp/qstar.lua" dry-run //:sysflags > "$tmp/sysflags.out" 2> "$tmp/sysflags.err"
contains "$tmp/sysflags.out" "-Llib"
contains "$tmp/sysflags.out" "-lm"

if "$qstar" --file "$tmp/qstar.lua" build //:plugin > "$tmp/shared.out" 2> "$tmp/shared.err"; then
	fail "sharedlib local executor unexpectedly succeeded"
fi
contains "$tmp/shared.err" "sharedlib targets are plan-only"

cat > "$tmp/src/test_pass.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/src/test_fail.c" <<'EOF'
int main(void) { return 3; }
EOF
cat > "$tmp/src/test_timeout.c" <<'EOF'
int main(void) { for (;;) {} return 0; }
EOF
cat > "$tmp/src/install_main.c" <<'EOF'
#include "core.h"
int main(void) { return core_value() - 13; }
EOF
cat > "$tmp/qstar.lua" <<'EOF'
qstar.test "unit_pass" {
  sources = {"src/test_pass.c"},
}

qstar.test "unit_fail" {
  sources = {"src/test_fail.c"},
}

qstar.test "unit_timeout" {
  sources = {"src/test_timeout.c"},
}

qstar.staticlib "install_core" {
  sources = {"src/core.c"},
  public_headers = {"include/core.h"},
  public_include_dirs = {"include"},
  private_include_dirs = {"src/core_private"},
}

qstar.exe "install_app" {
  sources = {"src/install_main.c"},
  deps = {"//:install_core"},
}
EOF

"$qstar" --file "$tmp/qstar.lua" test //:unit_pass > "$tmp/test-pass.out" 2> "$tmp/test-pass.err"
contains "$tmp/test-pass.out" "test_result label=//:unit_pass status=pass"
test -f "$tmp/.qstar/logs/___unit_pass.test.stdout" || fail "missing test stdout log"

if "$qstar" --file "$tmp/qstar.lua" test //:unit_fail > "$tmp/test-fail.out" 2> "$tmp/test-fail.err"; then
	fail "failing test unexpectedly succeeded"
fi
contains "$tmp/test-fail.out" "test_result label=//:unit_fail status=fail exit=3"

if "$qstar" --file "$tmp/qstar.lua" test //:unit_timeout > "$tmp/test-timeout.out" 2> "$tmp/test-timeout.err"; then
	fail "timeout test unexpectedly succeeded"
fi
contains "$tmp/test-timeout.out" "test_result label=//:unit_timeout status=timeout"

"$qstar" --file "$tmp/qstar.lua" build //:install_app > "$tmp/install-build.out" 2> "$tmp/install-build.err"
contains "$tmp/install-build.out" "status ok"
"$qstar" --file "$tmp/qstar.lua" install //:install_app --prefix "$tmp/prefix" --dry-run > "$tmp/install-dry.out" 2> "$tmp/install-dry.err"
contains "$tmp/install-dry.out" "mode dry-run"
contains "$tmp/install-dry.out" "install_file src=.qstar/out/___install_app/install_app"
"$qstar" --file "$tmp/qstar.lua" install //:install_core --prefix "$tmp/prefix" > "$tmp/install-lib.out" 2> "$tmp/install-lib.err"
contains "$tmp/install-lib.out" "status ok"
test -f "$tmp/prefix/lib/libinstall_core.a" || fail "missing installed staticlib"
test -f "$tmp/prefix/include/core.h" || fail "missing installed public header"

if "$qstar" --file "$tmp/qstar.lua" install //:unit_pass --prefix "$tmp/prefix" > "$tmp/install-test.out" 2> "$tmp/install-test.err"; then
	fail "non-installable test target unexpectedly installed"
fi
contains "$tmp/install-test.err" "not installable"

manual_root=$(pwd)/tests/manual

cp -R "$manual_root/c-only" "$tmp/c-only"
"$qstar" --file "$tmp/c-only/qstar.lua" build //:app > "$tmp/c-only-build.out" 2> "$tmp/c-only-build.err"
contains "$tmp/c-only-build.out" "status ok"
"$tmp/c-only/.qstar/out/___app/app"
contains "$tmp/c-only/compile_commands.json" "src/main.c"
contains "$tmp/c-only/compile_commands.json" "src/core.c"
"$qstar" --file "$tmp/c-only/qstar.lua" test //:unit > "$tmp/c-only-test.out" 2> "$tmp/c-only-test.err"
contains "$tmp/c-only-test.out" "test_result label=//:unit status=pass"
"$qstar" --file "$tmp/c-only/qstar.lua" install //:core --prefix "$tmp/c-only-prefix" > "$tmp/c-only-install.out" 2> "$tmp/c-only-install.err"
contains "$tmp/c-only-install.out" "status ok"
test -f "$tmp/c-only-prefix/lib/libcore.a" || fail "c-only sample did not install libcore.a"
test -f "$tmp/c-only-prefix/include/corpus.h" || fail "c-only sample did not install public header"
contains "$tmp/c-only/compile_commands.json" "tests/unit.c"
"$qstar" --file "$tmp/c-only/qstar.lua" clean > "$tmp/c-only-clean.out" 2> "$tmp/c-only-clean.err"
contains "$tmp/c-only-clean.out" "clean_all .qstar compile_commands.json"
"$qstar" --file "$tmp/c-only/qstar.lua" build //:app > "$tmp/c-only-rebuild.out" 2> "$tmp/c-only-rebuild.err"
contains "$tmp/c-only-rebuild.out" "status=run"
contains "$tmp/c-only-rebuild.out" "status ok"

cp -R "$manual_root/generated" "$tmp/generated-sample"
rm -rf "$tmp/generated-sample/.qstar" "$tmp/generated-sample/generated" "$tmp/generated-sample/compile_commands.json"
"$qstar" --file "$tmp/generated-sample/qstar.lua" build //:app > "$tmp/generated-sample-build.out" 2> "$tmp/generated-sample-build.err"
contains "$tmp/generated-sample-build.out" "status ok"
test -f "$tmp/generated-sample/generated/config.h" || fail "generated sample missing config header"
test -f "$tmp/generated-sample/generated/value.c" || fail "generated sample missing generated source"
"$tmp/generated-sample/.qstar/out/___app/app"
contains "$tmp/generated-sample/compile_commands.json" "src/main.c"
contains "$tmp/generated-sample/compile_commands.json" "generated/value.c"
"$qstar" --file "$tmp/generated-sample/qstar.lua" clean > "$tmp/generated-sample-clean.out" 2> "$tmp/generated-sample-clean.err"
"$qstar" --file "$tmp/generated-sample/qstar.lua" build //:app > "$tmp/generated-sample-rebuild.out" 2> "$tmp/generated-sample-rebuild.err"
contains "$tmp/generated-sample-rebuild.out" "status ok"

cp -R "$manual_root/mixed-cale" "$tmp/mixed-sample"
"$qstar" --file "$tmp/mixed-sample/qstar.lua" dry-run //:mixed > "$tmp/mixed-sample-dry.out" 2> "$tmp/mixed-sample-dry.err"
contains "$tmp/mixed-sample-dry.out" "argv=[cale, -c, src/main.c"
contains "$tmp/mixed-sample-dry.out" "argv=[cale, -c, src/plugin.cale"
contains "$tmp/mixed-sample-dry.out" "rule provider=native final_action=link output_group=exe"

contains "../docs/qstar/qstar-v0-seal.md" "qstar/tests/manual/c-only"
contains "../docs/qstar/qstar-v0-seal.md" "qstar/tests/manual/generated"
contains "../docs/qstar/qstar-v0-seal.md" "qstar/tests/manual/mixed-cale"
contains "../docs/qstar/qstar-v0-seal.md" "make -C qstar qstar-v0-release-tests"
contains "README.md" "docs/qstar/qstar-v0-seal.md"

"$qstar" init c-app "$tmp/init-c-app" > "$tmp/init-c-app.out" 2> "$tmp/init-c-app.err"
contains "$tmp/init-c-app.out" "qstar init v1"
contains "$tmp/init-c-app.out" "template c-app"
"$qstar" --file "$tmp/init-c-app/qstar.lua" build //:app > "$tmp/init-c-app-build.out" 2> "$tmp/init-c-app-build.err"
contains "$tmp/init-c-app-build.out" "status ok"
"$tmp/init-c-app/.qstar/out/___app/app"

"$qstar" init c-lib "$tmp/init-c-lib" > "$tmp/init-c-lib.out" 2> "$tmp/init-c-lib.err"
contains "$tmp/init-c-lib.out" "template c-lib"
"$qstar" --file "$tmp/init-c-lib/qstar.lua" test //:unit > "$tmp/init-c-lib-test.out" 2> "$tmp/init-c-lib-test.err"
contains "$tmp/init-c-lib-test.out" "test_result label=//:unit status=pass"
"$qstar" --file "$tmp/init-c-lib/qstar.lua" install //:core --prefix "$tmp/init-c-lib-prefix" > "$tmp/init-c-lib-install.out" 2> "$tmp/init-c-lib-install.err"
test -f "$tmp/init-c-lib-prefix/lib/libcore.a" || fail "init c-lib did not install static library"

"$qstar" init generated "$tmp/init-generated" > "$tmp/init-generated.out" 2> "$tmp/init-generated.err"
contains "$tmp/init-generated.out" "template generated"
"$qstar" --file "$tmp/init-generated/qstar.lua" build //:app > "$tmp/init-generated-build.out" 2> "$tmp/init-generated-build.err"
contains "$tmp/init-generated-build.out" "status ok"
test -f "$tmp/init-generated/generated/config.h" || fail "init generated missing config header"
"$tmp/init-generated/.qstar/out/___app/app"

"$qstar" init mixed-cale "$tmp/init-mixed" > "$tmp/init-mixed.out" 2> "$tmp/init-mixed.err"
contains "$tmp/init-mixed.out" "template mixed-cale"
"$qstar" --file "$tmp/init-mixed/qstar.lua" dry-run //:mixed > "$tmp/init-mixed-dry.out" 2> "$tmp/init-mixed-dry.err"
contains "$tmp/init-mixed-dry.out" "argv=[cale, -c, src/plugin.cale"

if "$qstar" init c-app "$tmp/init-c-app" > "$tmp/init-overwrite.out" 2> "$tmp/init-overwrite.err"; then
	fail "qstar init unexpectedly overwrote existing files"
fi
contains "$tmp/init-overwrite.err" "refuses to overwrite existing file"

"$qstar" --file "$tmp/init-c-lib/qstar.lua" explain //:core > "$tmp/rule-explain.out" 2> "$tmp/rule-explain.err"
contains "$tmp/rule-explain.out" "rule provider=native final_action=archive output_group=libs"
contains "$tmp/rule-explain.out" "source_file path=src/core.c language=c tool=c-compiler provider=c output_group=objects role=compile"

echo "qstar-smoke: passed"
