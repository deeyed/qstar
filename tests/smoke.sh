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

"$qstar" --file "$tmp/qstar.lua" build //:app --jobs 2 --schedule-trace > "$tmp/first.out" 2> "$tmp/first.err"
contains "$tmp/first.out" "qstar build v2"
contains "$tmp/first.out" "executor-policy version=v3"
contains "$tmp/first.out" "parallel=optional jobs=2"
contains "$tmp/first.out" "action_dag target=//:app"
contains "$tmp/first.out" "schedule_action id=//:app:compile:0"
contains "$tmp/first.out" "status=run"
contains "$tmp/first.out" "status ok"
test -f "$tmp/.qstar/state/actions.json" || fail "missing action state"
test -f "$tmp/.qstar/state/graph.json" || fail "missing graph snapshot"
test -f "$tmp/.qstar/state/last-summary.json" || fail "missing build summary"
contains "$tmp/.qstar/state/graph.json" "\"schema\":\"qstar-graph-snapshot-v1\""
contains "$tmp/.qstar/state/graph.json" "\"label\":\"//:app\""
contains "$tmp/.qstar/state/last-summary.json" "\"schema\":\"qstar-build-summary-v1\""
contains "$tmp/.qstar/state/last-summary.json" "\"status\":\"success\""
contains "$tmp/.qstar/state/actions.json" "\"argv_key\":"
contains "$tmp/.qstar/state/actions.json" "\"env_key\":"
contains "$tmp/.qstar/state/actions.json" "\"input_key\":"
test -f "$tmp/compile_commands.json" || fail "missing compile_commands.json"
contains "$tmp/compile_commands.json" "src/main.c"

"$qstar" --file "$tmp/qstar.lua" action-log //:app:compile:0 > "$tmp/action-log.out" 2> "$tmp/action-log.err"
contains "$tmp/action-log.out" "qstar action-log v1"
contains "$tmp/action-log.out" "action //:app:compile:0"
contains "$tmp/action-log.out" "qstar-action-log v2"
contains "$tmp/action-log.out" "argv[0]=cc"
"$qstar" --file "$tmp/qstar.lua" replay //:app:compile:0 > "$tmp/action-replay.out" 2> "$tmp/action-replay.err"
contains "$tmp/action-replay.out" "qstar replay v1"
contains "$tmp/action-replay.out" "action //:app:compile:0"
contains "$tmp/action-replay.out" "cc -c src/main.c"

"$qstar" --file "$tmp/qstar.lua" build //:app > "$tmp/second.out" 2> "$tmp/second.err"
contains "$tmp/second.out" "status=skip"

"$qstar" --file "$tmp/qstar.lua" why-rebuild //:app > "$tmp/why.out" 2> "$tmp/why.err"
contains "$tmp/why.out" "qstar why-rebuild v1"
contains "$tmp/why.out" "reason=output-check"
contains "$tmp/why.out" "status=skip"
rm -f "$tmp/.qstar/out/___app/obj0.o"
"$qstar" --file "$tmp/qstar.lua" why-rebuild //:app > "$tmp/why-output.out" 2> "$tmp/why-output.err"
contains "$tmp/why-output.out" "reason=output-missing"

cat > "$tmp/src/main.c" <<'EOF'
int main(void) { return 1 - 1; }
EOF

"$qstar" --file "$tmp/qstar.lua" build //:app --explain-cache > "$tmp/third.out" 2> "$tmp/third.err"
contains "$tmp/third.out" "cache_miss id=//:app:compile:0"
contains "$tmp/third.out" "reason=input-changed"
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
contains "$tmp/.qstar/state/last-summary.json" "\"status\":\"failure\""
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
qstar.genrule "bad_arg" {
  tool = "tools/gen-value.sh",
  outputs = {qstar.output("generated/safe.c")},
  args = {"../escape.c"},
}

qstar.exe "bad_gen" {
  sources = {qstar.output("generated/safe.c")},
}
EOF

if "$qstar" --file "$tmp/qstar.lua" build //:bad_gen > "$tmp/bad-arg.out" 2> "$tmp/bad-arg.err"; then
	fail "generated action outside arg unexpectedly succeeded"
fi
contains "$tmp/bad-arg.err" "escapes package root"

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
qstar.config_header "install_cfg" {
  output = qstar.output("generated/install_config.h"),
  defines = {"INSTALL_FEATURE=1"},
}

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
  public_headers = {"include/core.h", qstar.output("generated/install_config.h")},
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
contains "$tmp/install-dry.out" "install_diff dst=$tmp/prefix/bin/install_app action=would-create"
"$qstar" --file "$tmp/qstar.lua" install //:install_core --prefix "$tmp/prefix" > "$tmp/install-lib.out" 2> "$tmp/install-lib.err"
contains "$tmp/install-lib.out" "status ok"
test -f "$tmp/prefix/lib/libinstall_core.a" || fail "missing installed staticlib"
test -f "$tmp/prefix/include/core.h" || fail "missing installed public header"
test -f "$tmp/prefix/include/generated/install_config.h" || fail "missing installed generated public header"
test -f "$tmp/.qstar/install/manifest.json" || fail "missing install manifest"
contains "$tmp/.qstar/install/manifest.json" "\"schema\":\"qstar-install-manifest-v2\""
contains "$tmp/.qstar/install/manifest.json" "\"role\":\"staticlib\""
contains "$tmp/.qstar/install/manifest.json" "\"role\":\"header\""
contains "$tmp/.qstar/install/manifest.json" "\"cmake_config\":\"deferred\""

if "$qstar" --file "$tmp/qstar.lua" install //:unit_pass --prefix "$tmp/prefix" > "$tmp/install-test.out" 2> "$tmp/install-test.err"; then
	fail "non-installable test target unexpectedly installed"
fi
contains "$tmp/install-test.err" "not installable"

manual_root=$(pwd)/tests/manual
project_root=$(pwd)/tests/projects

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

cp -R "$project_root/c-app-lib-test" "$tmp/project-c"
"$qstar" --file "$tmp/project-c/qstar.lua" build //:app > "$tmp/project-c-build.out" 2> "$tmp/project-c-build.err"
contains "$tmp/project-c-build.out" "status ok"
"$tmp/project-c/.qstar/out/___app/app"
"$qstar" --file "$tmp/project-c/qstar.lua" test //:unit > "$tmp/project-c-test.out" 2> "$tmp/project-c-test.err"
contains "$tmp/project-c-test.out" "test_result label=//:unit status=pass"
"$qstar" --file "$tmp/project-c/qstar.lua" install //:core --prefix "$tmp/project-c-prefix" > "$tmp/project-c-install.out" 2> "$tmp/project-c-install.err"
test -f "$tmp/project-c-prefix/lib/libcore.a" || fail "project corpus c lib did not install"
test -f "$tmp/project-c-prefix/include/corpus.h" || fail "project corpus c header did not install"
contains "$tmp/project-c/.qstar/install/manifest.json" "\"schema\":\"qstar-install-manifest-v2\""
contains "$tmp/project-c/.qstar/install/manifest.json" "\"role\":\"staticlib\""
contains "$tmp/project-c/.qstar/install/manifest.json" "\"cmake_config\":\"deferred\""
contains "$tmp/project-c/compile_commands.json" "src/core.c"
contains "$tmp/project-c/compile_commands.json" "tests/unit.c"
contains "$tmp/project-c/.qstar/state/graph.json" "\"schema\":\"qstar-graph-snapshot-v1\""
contains "$tmp/project-c/.qstar/state/graph.json" "\"label\":\"//:app\""
contains "$tmp/project-c/.qstar/state/last-summary.json" "\"schema\":\"qstar-build-summary-v1\""
contains "$tmp/project-c/.qstar/state/last-summary.json" "\"status\":\"success\""
"$qstar" --file "$tmp/project-c/qstar.lua" build //:app > "$tmp/project-c-skip.out" 2> "$tmp/project-c-skip.err"
contains "$tmp/project-c-skip.out" "status=skip"
cat > "$tmp/project-c/src/main.c" <<'EOF'
#include "corpus.h"
int main(void) { return corpus_value() - 31; }
EOF
"$qstar" --file "$tmp/project-c/qstar.lua" build //:app --explain-cache > "$tmp/project-c-rebuild.out" 2> "$tmp/project-c-rebuild.err"
contains "$tmp/project-c-rebuild.out" "cache_miss id=//:app:compile:0"
contains "$tmp/project-c-rebuild.out" "status ok"

if command -v c++ >/dev/null 2>&1; then
	cp -R "$project_root/cxx-mixed" "$tmp/project-cxx"
	"$qstar" --file "$tmp/project-cxx/qstar.lua" build //:mixed --jobs 2 --schedule-trace > "$tmp/project-cxx-build.out" 2> "$tmp/project-cxx-build.err"
	contains "$tmp/project-cxx-build.out" "parallel_compile target=//:mixed jobs=2 sources=2 mode=process-v2"
	contains "$tmp/project-cxx-build.out" "parallel_batch target=//:mixed jobs=2 total=2 policy=fifo"
	contains "$tmp/project-cxx-build.out" "schedule_action id=//:mixed:compile:0"
	contains "$tmp/project-cxx-build.out" "schedule_action id=//:mixed:compile:1"
	contains "$tmp/project-cxx-build.out" "parallel_event target=//:mixed event=start id=//:mixed:compile:0"
	contains "$tmp/project-cxx-build.out" "parallel_event target=//:mixed event=finish"
	contains "$tmp/project-cxx-build.out" "status ok"
	"$tmp/project-cxx/.qstar/out/___mixed/mixed"
	contains "$tmp/project-cxx/compile_commands.json" "src/cpp.cpp"
	contains "$tmp/project-cxx/compile_commands.json" "src/main.c"
fi

mkdir -p "$tmp/fanout/src"
cat > "$tmp/fanout/src/a.c" <<'EOF'
#include "config.h"
int a_value(void) { return FANOUT_VALUE; }
EOF
cat > "$tmp/fanout/src/b.c" <<'EOF'
#include "config.h"
int b_value(void) { return FANOUT_VALUE + 1; }
EOF
cat > "$tmp/fanout/src/main.c" <<'EOF'
int a_value(void);
int b_value(void);
int main(void) { return a_value() + b_value() - 15; }
EOF
cat > "$tmp/fanout/qstar.lua" <<'EOF'
qstar.config_header "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"FANOUT_VALUE=7"},
}

qstar.exe "app" {
  sources = {"src/a.c", "src/b.c", "src/main.c"},
  private_headers = {qstar.output("generated/config.h")},
  include_dirs = {"generated"},
}
EOF
"$qstar" --file "$tmp/fanout/qstar.lua" build //:app --jobs 2 --schedule-trace > "$tmp/fanout-build.out" 2> "$tmp/fanout-build.err"
contains "$tmp/fanout-build.out" "build_action id=//:cfg:generate:0 status=run"
contains "$tmp/fanout-build.out" "parallel_compile target=//:app jobs=2 sources=3 mode=process-v2"
contains "$tmp/fanout-build.out" "parallel_slot target=//:app slot=0 state=assign action=//:app:compile:0 queue=0"
contains "$tmp/fanout-build.out" "parallel_slot target=//:app slot=1 state=assign action=//:app:compile:1 queue=1"
contains "$tmp/fanout-build.out" "action=//:app:compile:2 queue=2"
contains "$tmp/fanout-build.out" "status ok"

mkdir -p "$tmp/parallel-fail/src" "$tmp/parallel-fail/tools"
cat > "$tmp/parallel-fail/tools/fake-cc.sh" <<'EOF'
#!/bin/sh
set -eu
src=
out=
dep=
prev=
for arg in "$@"; do
  if [ "$prev" = "-o" ]; then out=$arg; prev=; continue; fi
  if [ "$prev" = "-MF" ]; then dep=$arg; prev=; continue; fi
  case "$arg" in
    -o) prev="-o" ;;
    -MF) prev="-MF" ;;
    *.c) src=$arg ;;
  esac
done
case "$src" in
  *slow.c) sleep 20 ;;
  *fail.c) echo "fake compiler failure" >&2; exit 9 ;;
esac
if [ "$dep" ]; then
  mkdir -p "$(dirname "$dep")"
  printf '%s: %s\n' "$out" "$src" > "$dep"
fi
cc -c "$src" -o "$out"
EOF
chmod +x "$tmp/parallel-fail/tools/fake-cc.sh"
cat > "$tmp/parallel-fail/src/slow.c" <<'EOF'
int slow_value(void) { return 1; }
EOF
cat > "$tmp/parallel-fail/src/fail.c" <<'EOF'
int fail_value(void) { return 2; }
EOF
cat > "$tmp/parallel-fail/src/ok.c" <<'EOF'
int ok_value(void) { return 3; }
EOF
cat > "$tmp/parallel-fail/Cale.toml" <<'EOF'
profile = "fake"

[profile.fake]
cc = "tools/fake-cc.sh"
EOF
cat > "$tmp/parallel-fail/qstar.lua" <<'EOF'
qstar.exe "race" {
  sources = {"src/slow.c", "src/fail.c", "src/ok.c"},
}
EOF
if "$qstar" --file "$tmp/parallel-fail/qstar.lua" build //:race --jobs 2 --schedule-trace > "$tmp/parallel-fail.out" 2> "$tmp/parallel-fail.err"; then
	fail "parallel failure unexpectedly succeeded"
fi
contains "$tmp/parallel-fail.out" "parallel_compile target=//:race jobs=2 sources=3 mode=process-v2"
contains "$tmp/parallel-fail.out" "parallel_batch target=//:race jobs=2 total=3 policy=fifo"
contains "$tmp/parallel-fail.out" "parallel_slot target=//:race slot=0 state=assign action=//:race:compile:0 queue=0"
contains "$tmp/parallel-fail.out" "parallel_slot target=//:race slot=1 state=assign action=//:race:compile:1 queue=1"
contains "$tmp/parallel-fail.out" "build_action id=//:race:compile:1 status=fail exit=9"
contains "$tmp/parallel-fail.out" "parallel_event target=//:race event=fail id=//:race:compile:1 slot=1 exit=9 state=failed retry=next-build cancel=active"
contains "$tmp/parallel-fail.out" "build_action id=//:race:compile:0 status=cancelled reason=parallel-failure retry=next-build"
contains "$tmp/parallel-fail.out" "parallel_event target=//:race event=cancel id=//:race:compile:0 slot=0 state=cancelled reason=parallel-failure retry=next-build"
contains "$tmp/parallel-fail/.qstar/logs/___race_compile_1.log" "qstar-action-log v2"
contains "$tmp/parallel-fail/.qstar/logs/___race_compile_1.log" "argv[0]=tools/fake-cc.sh"
contains "$tmp/parallel-fail/.qstar/logs/last-failure.replay" "argv_digest="

mkdir -p "$tmp/parallel-timeout/src" "$tmp/parallel-timeout/tools"
cat > "$tmp/parallel-timeout/tools/fake-cc.sh" <<'EOF'
#!/bin/sh
set -eu
sleep 5
EOF
chmod +x "$tmp/parallel-timeout/tools/fake-cc.sh"
cat > "$tmp/parallel-timeout/src/timeout.c" <<'EOF'
int timeout_value(void) { return 1; }
EOF
cat > "$tmp/parallel-timeout/src/other.c" <<'EOF'
int other_value(void) { return 2; }
EOF
cat > "$tmp/parallel-timeout/Cale.toml" <<'EOF'
profile = "fake"

[profile.fake]
cc = "tools/fake-cc.sh"
EOF
cat > "$tmp/parallel-timeout/qstar.lua" <<'EOF'
qstar.exe "timeout" {
  sources = {"src/timeout.c", "src/other.c"},
}
EOF
if QSTAR_TEST_ACTION_TIMEOUT_SEC=1 "$qstar" --file "$tmp/parallel-timeout/qstar.lua" build //:timeout --jobs 2 --schedule-trace > "$tmp/parallel-timeout.out" 2> "$tmp/parallel-timeout.err"; then
	fail "parallel timeout unexpectedly succeeded"
fi
contains "$tmp/parallel-timeout.out" "executor-policy version=v3 parallel=optional jobs=2 active=compile-process-v2 failure=stop-on-first-failure action_timeout_sec=1"
contains "$tmp/parallel-timeout.out" "parallel_compile target=//:timeout jobs=2 sources=2 mode=process-v2"
contains "$tmp/parallel-timeout.out" "parallel_event target=//:timeout event=timeout id=//:timeout:compile:0 slot=0 state=timeout retry=next-build cancel=active"
contains "$tmp/parallel-timeout.out" "build_action id=//:timeout:compile:1 status=cancelled reason=parallel-failure retry=next-build"
contains "$tmp/parallel-timeout.out" "parallel_event target=//:timeout event=cancel id=//:timeout:compile:1 slot=1 state=cancelled reason=parallel-failure retry=next-build"
contains "$tmp/parallel-timeout.out" "cancel_propagation policy=stop-on-first-failure"
contains "$tmp/parallel-timeout/.qstar/logs/last-failure.replay" "argv_digest="

cp -R "$project_root/generated-config" "$tmp/project-generated"
"$qstar" --file "$tmp/project-generated/qstar.lua" build //:app > "$tmp/project-generated-build.out" 2> "$tmp/project-generated-build.err"
contains "$tmp/project-generated-build.out" "status ok"
test -f "$tmp/project-generated/generated/config.h" || fail "project corpus generated config missing"
test -f "$tmp/project-generated/generated/value.c" || fail "project corpus generated source missing"
"$tmp/project-generated/.qstar/out/___app/app"
contains "$tmp/project-generated/compile_commands.json" "generated/value.c"
contains "$tmp/project-generated/.qstar/state/graph.json" "\"schema\":\"qstar-graph-snapshot-v1\""
"$qstar" --file "$tmp/project-generated/qstar.lua" build //:app > "$tmp/project-generated-skip.out" 2> "$tmp/project-generated-skip.err"
contains "$tmp/project-generated-skip.out" "status=skip"

cp -R "$project_root/multipkg" "$tmp/project-multipkg"
"$qstar" --file "$tmp/project-multipkg/qstar.lua" build //app:app > "$tmp/project-multipkg-build.out" 2> "$tmp/project-multipkg-build.err"
contains "$tmp/project-multipkg-build.out" "package-root $tmp/project-multipkg"
contains "$tmp/project-multipkg-build.out" "status ok"
"$tmp/project-multipkg/.qstar/out/__app_app/app"
"$qstar" --file "$tmp/project-multipkg/qstar.lua" install //lib:core --prefix "$tmp/project-multipkg-prefix" > "$tmp/project-multipkg-install.out" 2> "$tmp/project-multipkg-install.err"
test -f "$tmp/project-multipkg-prefix/lib/libcore.a" || fail "multipkg corpus lib did not install"
test -f "$tmp/project-multipkg-prefix/include/core.h" || fail "multipkg corpus header did not install"
contains "$tmp/project-multipkg/.qstar/install/manifest.json" "\"schema\":\"qstar-install-manifest-v2\""
contains "$tmp/project-multipkg/compile_commands.json" "lib/src/core.c"
contains "$tmp/project-multipkg/compile_commands.json" "app/src/main.c"

contains "../docs/qstar/qstar-v0-seal.md" "qstar/tests/manual/c-only"
contains "../docs/qstar/qstar-v0-seal.md" "qstar/tests/manual/generated"
contains "../docs/qstar/qstar-v0-seal.md" "qstar/tests/manual/mixed-cale"
contains "../docs/qstar/qstar-v0-seal.md" "make -C qstar qstar-v0-release-tests"
contains "../docs/qstar/qstar-v0-seal.md" "qstar-v0.1-release-tests"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "status: v0.1 standalone developer build system"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "make -C qstar qstar-v0.1-release-tests"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/c-app-lib-test"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/cxx-mixed"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/generated-config"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/multipkg"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "Cale build integration: deferred"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "qstar action-log <action-id>"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "qstar replay <action-id>"
contains "README.md" "docs/qstar/qstar-v0-seal.md"
contains "README.md" "docs/qstar/qstar-v0.1-hardening-seal.md"
contains "README.md" "qstar-v0.1-release-tests"

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

mkdir -p "$tmp/depfile/include" "$tmp/depfile/src"
cat > "$tmp/depfile/include/dep.h" <<'EOF'
#define DEP_VALUE 11
EOF
cat > "$tmp/depfile/src/main.c" <<'EOF'
#include "dep.h"
int main(void) { return DEP_VALUE - 11; }
EOF
cat > "$tmp/depfile/qstar.lua" <<'EOF'
qstar.exe "app" {
  sources = {"src/main.c"},
  include_dirs = {"include"},
}
EOF
"$qstar" --file "$tmp/depfile/qstar.lua" build //:app > "$tmp/depfile-first.out" 2> "$tmp/depfile-first.err"
contains "$tmp/depfile-first.out" "status ok"
test -f "$tmp/depfile/.qstar/out/___app/obj0.d" || fail "missing compiler depfile"
cat > "$tmp/depfile/include/dep.h" <<'EOF'
#define DEP_VALUE 12
EOF
"$qstar" --file "$tmp/depfile/qstar.lua" build //:app --explain-cache > "$tmp/depfile-second.out" 2> "$tmp/depfile-second.err"
contains "$tmp/depfile-second.out" "cache_miss id=//:app:compile:0"
contains "$tmp/depfile-second.out" "reason=depfile-changed"
rm -f "$tmp/depfile/include/dep.h"
if "$qstar" --file "$tmp/depfile/qstar.lua" build //:app > "$tmp/depfile-missing.out" 2> "$tmp/depfile-missing.err"; then
	fail "missing depfile-discovered header unexpectedly succeeded"
fi
contains "$tmp/depfile-missing.err" "depfile-discovered header"

if command -v c++ >/dev/null 2>&1; then
	mkdir -p "$tmp/cxx/include" "$tmp/cxx/src"
	cat > "$tmp/cxx/include/cpp.hpp" <<'EOF'
#ifndef QSTAR_CXX_FLAG
#error missing C++ flag
#endif
static inline int qstar_cpp_value(void) { return 37 + QSTAR_CXX_FLAG; }
EOF
	cat > "$tmp/cxx/src/cpp.cpp" <<'EOF'
#include "cpp.hpp"
extern "C" int cpp_value(void) { return qstar_cpp_value(); }
EOF
	cat > "$tmp/cxx/src/main.c" <<'EOF'
#ifndef QSTAR_C_FLAG
#error missing C flag
#endif
int cpp_value(void);
int main(void) { return cpp_value() - 42; }
EOF
	cat > "$tmp/cxx/qstar.lua" <<'EOF'
qstar.exe "mixed" {
  sources = {"src/main.c", "src/cpp.cpp"},
  include_dirs = {"include"},
  cflags = {"-DQSTAR_C_FLAG=1"},
  cxxflags = {"-DQSTAR_CXX_FLAG=5"},
  cxx_standard = "c++11",
}
EOF
	"$qstar" --file "$tmp/cxx/qstar.lua" dry-run //:mixed > "$tmp/cxx-dry.out" 2> "$tmp/cxx-dry.err"
	contains "$tmp/cxx-dry.out" "argv=[c++, -c, src/cpp.cpp"
	contains "$tmp/cxx-dry.out" "-std=c++11"
	contains "$tmp/cxx-dry.out" "-DQSTAR_CXX_FLAG=5"
	contains "$tmp/cxx-dry.out" "argv=[c++, -o, .qstar/out/___mixed/mixed"
	"$qstar" --file "$tmp/cxx/qstar.lua" build //:mixed > "$tmp/cxx-build.out" 2> "$tmp/cxx-build.err"
	contains "$tmp/cxx-build.out" "status ok"
	"$tmp/cxx/.qstar/out/___mixed/mixed"
	contains "$tmp/cxx/compile_commands.json" "src/cpp.cpp"
fi

cat > "$tmp/cxx-module.qstar.lua" <<'EOF'
qstar.exe "bad_module" {
  sources = {"src/module.cppm"},
}
EOF
cat > "$tmp/src/module.cppm" <<'EOF'
export module bad;
EOF
if "$qstar" --file "$tmp/cxx-module.qstar.lua" build //:bad_module > "$tmp/cxx-module.out" 2> "$tmp/cxx-module.err"; then
	fail "C++ module source unexpectedly built"
fi
contains "$tmp/cxx-module.err" "C++ modules are not supported"

mkdir -p "$tmp/workspace/app/src" "$tmp/workspace/lib/src" "$tmp/workspace/lib/include" "$tmp/workspace/lib/private"
touch "$tmp/workspace/qstar.workspace"
cat > "$tmp/workspace/lib/include/core.h" <<'EOF'
int core_value(void);
EOF
cat > "$tmp/workspace/lib/private/core_private.h" <<'EOF'
#define CORE_PRIVATE 1
EOF
cat > "$tmp/workspace/lib/src/core.c" <<'EOF'
#include "core.h"
int core_value(void) { return 5; }
EOF
cat > "$tmp/workspace/app/src/main.c" <<'EOF'
#include "core.h"
int main(void) { return core_value() - 5; }
EOF
cat > "$tmp/workspace/lib/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  sources = {"lib/src/core.c"},
  public_headers = {"lib/include/core.h"},
  private_headers = {"lib/private/core_private.h"},
  public_include_dirs = {"lib/include"},
  private_include_dirs = {"lib/private"},
  visibility = {"//app:..."},
}
EOF
cat > "$tmp/workspace/app/qstar.lua" <<'EOF'
qstar.exe "app" {
  sources = {"app/src/main.c"},
  deps = {"//lib:core"},
}
EOF
cat > "$tmp/workspace/qstar.lua" <<'EOF'
qstar.subdir("lib")
qstar.subdir("app")
EOF
"$qstar" --file "$tmp/workspace/app/qstar.lua" query //app:app > "$tmp/workspace-query.out" 2> "$tmp/workspace-query.err"
contains "$tmp/workspace-query.out" "target //app:app"
contains "$tmp/workspace-query.out" "package app"
"$qstar" --file "$tmp/workspace/qstar.lua" build //app:app > "$tmp/workspace-build.out" 2> "$tmp/workspace-build.err"
contains "$tmp/workspace-build.out" "package-root $tmp/workspace"
contains "$tmp/workspace-build.out" "status ok"
"$tmp/workspace/.qstar/out/__app_app/app"

cat > "$tmp/workspace/app/qstar.lua" <<'EOF'
qstar.exe "bad_leak" {
  sources = {"app/src/main.c"},
  deps = {"//lib:core"},
  include_dirs = {"lib/private"},
}
EOF
if "$qstar" --file "$tmp/workspace/qstar.lua" check //app:bad_leak > "$tmp/private-leak.out" 2> "$tmp/private-leak.err"; then
	fail "private include leakage unexpectedly succeeded"
fi
contains "$tmp/private-leak.err" "leaks private include directory"

cat > "$tmp/workspace/lib/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  sources = {"lib/src/core.c"},
  public_headers = {"lib/include/core.h"},
  public_include_dirs = {"lib/include"},
  visibility = {"//other:..."},
}
EOF
cat > "$tmp/workspace/app/qstar.lua" <<'EOF'
qstar.exe "blocked" {
  sources = {"app/src/main.c"},
  deps = {"//lib:core"},
}
EOF
if "$qstar" --file "$tmp/workspace/qstar.lua" check //app:blocked > "$tmp/visibility.out" 2> "$tmp/visibility.err"; then
	fail "visibility violation unexpectedly succeeded"
fi
contains "$tmp/visibility.err" "is not visible"

cat > "$tmp/workspace/app/qstar.lua" <<'EOF'
qstar.exe "//other:oops" {
  sources = {"app/src/main.c"},
}
EOF
if "$qstar" --file "$tmp/workspace/app/qstar.lua" check //other:oops > "$tmp/ownership.out" 2> "$tmp/ownership.err"; then
	fail "cross-package ownership unexpectedly succeeded"
fi
contains "$tmp/ownership.err" "owned by package"

cat > "$tmp/workspace/app/qstar.lua" <<'EOF'
qstar.exe "outside" {
  sources = {"../outside.c"},
}
EOF
if "$qstar" --file "$tmp/workspace/app/qstar.lua" check //app:outside > "$tmp/outside-source.out" 2> "$tmp/outside-source.err"; then
	fail "outside source path unexpectedly succeeded"
fi
contains "$tmp/outside-source.err" "must be package-relative"

mkdir -p "$tmp/profile/.cale/profiles" "$tmp/profile/src"
cat > "$tmp/profile/Cale.toml" <<'EOF'
profile = "custom"

[profile.custom]
toolchain = "clang"
target = "x86_64-unknown-none-elf"
stdlib = "none"
cc = "clang-custom"
cxx = "clang++-custom"
cale = "cale-custom"
ar = "llvm-ar-custom"
linker = "ld-custom"
sysroot = "sdk root"
resource_dir = "resource dir"
include_dirs = ["profile include", "profinc"]
lib_dirs = ["profile lib"]
EOF
cat > "$tmp/profile/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/profile/qstar.lua" <<'EOF'
qstar.exe "app" {
  sources = {"src/main.c"},
  libs = {"m"},
}
EOF
"$qstar" --file "$tmp/profile/qstar.lua" dry-run //:app > "$tmp/profile-dry.out" 2> "$tmp/profile-dry.err"
contains "$tmp/profile-dry.out" "resolved_toolchain owner=//:app toolchain=clang profile=custom target=x86_64-unknown-none-elf stdlib=none resolver=profile-schema-v2 cc=clang-custom"
contains "$tmp/profile-dry.out" "\"--sysroot=sdk root\""
contains "$tmp/profile-dry.out" "-resource-dir"
contains "$tmp/profile-dry.out" "\"resource dir\""
contains "$tmp/profile-dry.out" "\"profile include\""
contains "$tmp/profile-dry.out" "\"-Lprofile lib\""
contains "$tmp/profile-dry.out" "digest="
"$qstar" --file "$tmp/profile/qstar.lua" doctor > "$tmp/profile-doctor.out" 2> "$tmp/profile-doctor.err"
contains "$tmp/profile-doctor.out" "profile-schema v2 include_dirs=2 lib_dirs=1"
contains "$tmp/profile-doctor.out" "toolchain-sanity name=clang cc=clang-custom cxx=clang++-custom cale=cale-custom ar=llvm-ar-custom linker=ld-custom"

mkdir -p "$tmp/longcmd/src"
cat > "$tmp/longcmd/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/longcmd/qstar.lua" <<'EOF'
qstar.exe "app" {
  sources = {"src/main.c"},
  include_dirs = {
    "include/very/long/path/segment/000",
    "include/very/long/path/segment/001",
    "include/very/long/path/segment/002",
    "include/very/long/path/segment/003",
    "include/very/long/path/segment/004",
    "include/very/long/path/segment/005",
    "include/very/long/path/segment/006",
    "include/very/long/path/segment/007",
    "include/very/long/path/segment/008",
    "include/very/long/path/segment/009",
    "include/very/long/path/segment/010",
    "include/very/long/path/segment/011",
    "include/very/long/path/segment/012",
    "include/very/long/path/segment/013",
    "include/very/long/path/segment/014",
    "include/very/long/path/segment/015",
    "include/very/long/path/segment/016",
    "include/very/long/path/segment/017",
    "include/very/long/path/segment/018",
    "include/very/long/path/segment/019",
  },
}
EOF
"$qstar" --file "$tmp/longcmd/qstar.lua" dry-run //:app > "$tmp/longcmd-dry.out" 2> "$tmp/longcmd-dry.err"
contains "$tmp/longcmd-dry.out" "response=skeleton"
contains "$tmp/longcmd-dry.out" "response_file=.qstar/rsp/"
contains "$tmp/longcmd-dry.out" "response_style=posix"
contains "$tmp/longcmd-dry.out" "response_digest="
"$qstar" --file "$tmp/longcmd/qstar.lua" build //:app > "$tmp/longcmd-build.out" 2> "$tmp/longcmd-build.err"
contains "$tmp/longcmd-build.out" "response_file id=//:app:compile:0"
contains "$tmp/longcmd-build.out" "style=posix"
contains "$tmp/longcmd-build.out" "digest="
test -d "$tmp/longcmd/.qstar/rsp" || fail "missing real response file dir"
contains "$tmp/longcmd-build.out" "status ok"

cat > "$tmp/longcmd/src/main.c" <<'EOF'
int main(void) { return ; }
EOF
if "$qstar" --file "$tmp/longcmd/qstar.lua" build //:app --explain-cache > "$tmp/longcmd-fail.out" 2> "$tmp/longcmd-fail.err"; then
	fail "long response-file failure unexpectedly succeeded"
fi
contains "$tmp/longcmd/.qstar/logs/last-failure.replay" "qstar failure replay v2"
contains "$tmp/longcmd/.qstar/logs/last-failure.replay" "argv_digest="
contains "$tmp/longcmd/.qstar/logs/last-failure.replay" "response_file path=.qstar/rsp/___app_compile_0.rsp style=posix digest="

mkdir -p "$tmp/rsppolicy/src"
cat > "$tmp/rsppolicy/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/rsppolicy/Cale.toml" <<'EOF'
profile = "norsp"

[profile.norsp]
toolchain = "clang"
target = "x86_64-unknown-none-elf"
response_files = "off"
EOF
cat > "$tmp/rsppolicy/qstar.lua" <<'EOF'
qstar.exe "app" {
  sources = {"src/main.c"},
  include_dirs = {
    "include/very/long/path/segment/000",
    "include/very/long/path/segment/001",
    "include/very/long/path/segment/002",
    "include/very/long/path/segment/003",
    "include/very/long/path/segment/004",
    "include/very/long/path/segment/005",
    "include/very/long/path/segment/006",
    "include/very/long/path/segment/007",
    "include/very/long/path/segment/008",
    "include/very/long/path/segment/009",
    "include/very/long/path/segment/010",
    "include/very/long/path/segment/011",
  },
}
EOF
"$qstar" --file "$tmp/rsppolicy/qstar.lua" dry-run //:app > "$tmp/rsppolicy-dry.out" 2> "$tmp/rsppolicy-dry.err"
contains "$tmp/rsppolicy-dry.out" "response=unsupported response_capability=off"
contains "$tmp/rsppolicy-dry.out" "response_files=off response_style=posix"

mkdir -p "$tmp/windows/src"
cat > "$tmp/windows/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/windows/Cale.toml" <<'EOF'
profile = "msvc"

[profile.msvc]
toolchain = "clang"
target = "x86_64-pc-windows-msvc"
cc = "clang-cl"
cxx = "clang-cl"
linker = "clang-cl"
response_style = "msvc"
EOF
cat > "$tmp/windows/qstar.lua" <<'EOF'
qstar.exe "app" {
  sources = {"src/main.c"},
  include_dirs = {
    "win include dir",
    "sdk\\include\\tail\\",
    "include/very/long/path/segment/000",
    "include/very/long/path/segment/001",
    "include/very/long/path/segment/002",
    "include/very/long/path/segment/003",
    "include/very/long/path/segment/004",
    "include/very/long/path/segment/005",
    "include/very/long/path/segment/006",
    "include/very/long/path/segment/007",
    "include/very/long/path/segment/008",
    "include/very/long/path/segment/009",
  },
  lib_dirs = {"win lib"},
  libs = {"user32"},
}
EOF
"$qstar" --file "$tmp/windows/qstar.lua" dry-run //:app > "$tmp/windows-dry.out" 2> "$tmp/windows-dry.err"
contains "$tmp/windows-dry.out" "response_style=msvc"
contains "$tmp/windows-dry.out" "response_digest="
contains "$tmp/windows-dry.out" "/link"
contains "$tmp/windows-dry.out" "/LIBPATH:win lib"
contains "$tmp/windows-dry.out" "user32.lib"

echo "qstar-smoke: passed"
