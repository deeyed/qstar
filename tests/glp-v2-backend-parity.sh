#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
case "$qstar" in
  /*) ;;
  *) qstar=$(pwd)/$qstar ;;
esac
fixture=tests/projects/glp-v2-backend-parity
tmp=${TMPDIR:-/tmp}/qstar-glp-v2-backend-parity.$$
last_step=setup

fail() {
  echo "qstar-glp-v2-backend-parity: $* (step=$last_step)" >&2
  exit 1
}

contains() {
  file=$1
  pattern=$2
  grep -F -q -- "$pattern" "$file" ||
    fail "missing '$pattern' in $file"
}

not_contains() {
  file=$1
  pattern=$2
  if grep -F -q -- "$pattern" "$file"; then
    fail "unexpected '$pattern' in $file"
  fi
}

field_from_line() {
  file=$1
  line_pattern=$2
  field=$3
  awk -v line_pattern="$line_pattern" -v field="$field" '
    index($0, line_pattern) {
      for (i = 1; i <= NF; i++) {
        if (index($i, field "=") == 1) {
          sub("^" field "=", "", $i)
          print $i
          exit
        }
      }
    }
  ' "$file"
}

line_value() {
  file=$1
  field=$2
  sed -n "s/^${field}=//p" "$file" | tail -n 1
}

cleanup() {
  rc=$?
  trap - EXIT HUP INT TERM
  if [ "$rc" -ne 0 ]; then
    echo "qstar-glp-v2-backend-parity: failed with exit $rc (step=$last_step)" >&2
    find "$tmp" -type f \( -name '*.out' -o -name '*.err' \) -print 2>/dev/null |
      while IFS= read -r file; do
        echo "--- $file" >&2
        tail -n 80 "$file" >&2 || true
      done
  fi
  rm -rf "$tmp"
  exit "$rc"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmp"
project=$tmp/stella
ninja_project=$tmp/ninja
cp -R "$fixture" "$project"
cp -R "$fixture" "$ninja_project"
chmod +x "$project/tools/fake-provider.sh" "$ninja_project/tools/fake-provider.sh"

last_step=graph
"$qstar" --file "$project/qstar.lua" check //... > "$tmp/check.out" 2> "$tmp/check.err"
"$qstar" --file "$project/qstar.lua" --dump-graph > "$tmp/graph.out" 2> "$tmp/graph.err"
contains "$tmp/graph.out" "language_provider namespace=zig id=zig api=qstar.lang/2"
contains "$tmp/graph.out" "language_provider namespace=rust id=rust api=qstar.lang/2"
contains "$tmp/graph.out" "language_provider namespace=cuda id=cuda api=qstar.lang/2"
contains "$tmp/graph.out" "provider_action phase=final api=qstar.lang/2 provider=zig"
contains "$tmp/graph.out" "provider_action phase=compile api=qstar.lang/2 provider=rust"
contains "$tmp/graph.out" "env_names=[QSTAR_GLP_V2_ENV, QSTAR_GLP_V2_CACHE]"
contains "$tmp/graph.out" "wants_depfile=true"

"$qstar" --file "$project/qstar.lua" query //:mixed --format json > "$tmp/query.json" 2> "$tmp/query.err"
contains "$tmp/query.json" '"provider_actions":['
contains "$tmp/query.json" '"phase":"compile","api":"qstar.lang/2","provider":"rust"'
contains "$tmp/query.json" '"phase":"compile","api":"qstar.lang/2","provider":"cuda"'
contains "$tmp/query.json" '"phase":"final","api":"qstar.lang/2","provider":"zig"'
contains "$tmp/query.json" '"env_names":["QSTAR_GLP_V2_ENV","QSTAR_GLP_V2_CACHE"]'
contains "$tmp/query.json" '"depfile":"build/qstar/out/___mixed/mixed.d","wants_depfile":true'
contains "$tmp/query.json" '"build/qstar/out/___rust_archive/librust_archive.a"'
contains "$tmp/query.json" '.import"'

last_step=explain
"$qstar" --file "$project/qstar.lua" explain //:mixed > "$tmp/explain.out" 2> "$tmp/explain.err"
contains "$tmp/explain.out" "provider_action_contract id=//:mixed:compile:1 phase=compile api=qstar.lang/2 provider=rust"
contains "$tmp/explain.out" "provider_action_contract id=//:mixed:compile:2 phase=compile api=qstar.lang/2 provider=cuda"
contains "$tmp/explain.out" "provider_action_contract id=//:mixed:link:0 phase=final api=qstar.lang/2 provider=zig inputs=7 outputs=4"
contains "$tmp/explain.out" "env_names=[QSTAR_GLP_V2_ENV, QSTAR_GLP_V2_CACHE]"
contains "$tmp/explain.out" "wants_depfile=true"
contains "$tmp/explain.out" "response=skeleton response_file=build/qstar/rsp/___mixed_link_0.rsp"
contains "$tmp/explain.out" "exec_argc=2"
"$qstar" --file "$ninja_project/qstar.lua" -B build-ninja -G ninja \
  explain //:mixed > "$tmp/ninja-explain.out" 2> "$tmp/ninja-explain.err"
contains "$tmp/ninja-explain.out" \
  "response=skeleton response_file=build-ninja/rsp/___mixed_link_0.rsp"
contains "$tmp/ninja-explain.out" "exec_argc=2"

last_step=stella-build
"$qstar" --file "$project/qstar.lua" --progress off build //:all > "$tmp/stella.out" 2> "$tmp/stella.err"
contains "$tmp/stella.out" "response_file id=//:mixed:link:0"
contains "$tmp/stella.out" "status ok run=7 skip=0 fail=0"
test -f "$project/build/qstar/out/___mixed/mixed" || fail "missing Stella primary runtime"
test -f "$project/build/qstar/out/___mixed/mixed.metadata" || fail "missing Stella metadata"
test -f "$project/build/qstar/out/___mixed/mixed.import" || fail "missing Stella link interface"
test -f "$project/build/qstar/out/___mixed/mixed.resources/index.txt" || fail "missing Stella tree artifact"
test -f "$project/build/qstar/out/___consumer/consumer.metadata" || fail "missing Stella consumer metadata"
contains "$project/build/qstar/out/___mixed/mixed.metadata" "env=alpha"
contains "$project/build/qstar/out/___mixed/mixed.metadata" "interface=build/qstar/out/___rust_archive/librust_archive.a"
contains "$project/build/qstar/out/___mixed/mixed.metadata" ".import"
contains "$project/build/qstar/out/___consumer/consumer.metadata" "interface=build/qstar/out/___mixed/mixed.import"

last_step=compile-commands
contains "$project/build/qstar/compile_commands.json" '"file":"src/support.rs"'
contains "$project/build/qstar/compile_commands.json" '"file":"src/kernel.cu"'
contains "$project/build/qstar/compile_commands.json" '"file":"src/native.c"'
not_contains "$project/build/qstar/compile_commands.json" '"file":"src/main.zig"'

last_step=stella-observability
"$qstar" --file "$project/qstar.lua" action-log //:mixed:link:0 > "$tmp/stella-log.out" 2> "$tmp/stella-log.err"
contains "$tmp/stella-log.out" "backend=stella"
contains "$tmp/stella-log.out" "env[0]=QSTAR_GLP_V2_ENV=<redacted>"
contains "$tmp/stella-log.out" "output_count=4"
contains "$tmp/stella-log.out" "output[2]=build/qstar/out/___mixed/mixed.resources"
contains "$tmp/stella-log.out" "response_file=build/qstar/rsp/___mixed_link_0.rsp"
contains "$tmp/stella-log.out" "response_style=posix"
contains "$tmp/stella-log.out" "exec_argc=2"
stella_plan_digest=$(field_from_line "$tmp/explain.out" \
  "command_argv id=//:mixed:link:0" "digest")
stella_log_digest=$(line_value "$tmp/stella-log.out" "logical_argv_digest")
test "$stella_plan_digest" = "$stella_log_digest" ||
  fail "GLP v2 Stella logical argv digest mismatch"
stella_plan_rsp_digest=$(field_from_line "$tmp/explain.out" \
  "command_argv id=//:mixed:link:0" "response_digest")
stella_log_rsp_digest=$(line_value "$tmp/stella-log.out" "response_digest")
test "$stella_plan_rsp_digest" = "$stella_log_rsp_digest" ||
  fail "GLP v2 Stella response digest mismatch"
"$qstar" --file "$project/qstar.lua" replay //:mixed:link:0 > "$tmp/stella-replay.out" 2> "$tmp/stella-replay.err"
contains "$tmp/stella-replay.out" "env[1]=QSTAR_GLP_V2_CACHE=<redacted>"
contains "$tmp/stella-replay.out" "output_count=4"
contains "$tmp/stella-replay.out" "output[3]=build/qstar/out/___mixed/mixed.import"
contains "$tmp/stella-replay.out" "response_file=build/qstar/rsp/___mixed_link_0.rsp"
"$qstar" --file "$project/qstar.lua" --progress off build //:all > "$tmp/stella-repeat.out" 2> "$tmp/stella-repeat.err"
contains "$tmp/stella-repeat.out" "status ok run=0 skip=7 fail=0"

last_step=depfile-cache
printf '%s\n' "glp-v2-depfile-input-v2" > "$project/src/tracked.input"
"$qstar" --file "$project/qstar.lua" why-rebuild //:mixed > "$tmp/depfile-why.out" 2> "$tmp/depfile-why.err"
contains "$tmp/depfile-why.out" "cache_action id=//:mixed:link:0 kind=link status=run reason=depfile-changed"
"$qstar" --file "$project/qstar.lua" --progress off build //:all > "$tmp/depfile-build.out" 2> "$tmp/depfile-build.err"
contains "$tmp/depfile-build.out" "status ok"

last_step=env-cache
sed 's/env_value = "alpha"/env_value = "beta"/' "$project/qstar.lua" > "$project/qstar.lua.tmp"
mv "$project/qstar.lua.tmp" "$project/qstar.lua"
"$qstar" --file "$project/qstar.lua" why-rebuild //:mixed > "$tmp/env-why.out" 2> "$tmp/env-why.err"
contains "$tmp/env-why.out" "cache_action id=//:mixed:link:0 kind=link status=run reason=env-changed"
"$qstar" --file "$project/qstar.lua" --progress off build //:mixed > "$tmp/env-build.out" 2> "$tmp/env-build.err"
contains "$project/build/qstar/out/___mixed/mixed.metadata" "env=beta"

last_step=ninja-build
command -v ninja >/dev/null 2>&1 || fail "ninja is required"
"$qstar" --file "$ninja_project/qstar.lua" -B build-ninja -G ninja build //:all > "$tmp/ninja.out" 2> "$tmp/ninja.err"
contains "$tmp/ninja.out" "backend ninja"
contains "$tmp/ninja.out" "status ok"
test -f "$ninja_project/build-ninja/out/___mixed/mixed" || fail "missing Ninja primary runtime"
test -f "$ninja_project/build-ninja/out/___mixed/mixed.metadata" || fail "missing Ninja metadata"
test -f "$ninja_project/build-ninja/out/___mixed/mixed.import" || fail "missing Ninja link interface"
test -f "$ninja_project/build-ninja/out/___mixed/mixed.resources/index.txt" || fail "missing Ninja tree artifact"
contains "$ninja_project/build-ninja/compile_commands.json" '"file":"src/support.rs"'
contains "$ninja_project/build-ninja/compile_commands.json" '"file":"src/kernel.cu"'
contains "$ninja_project/build-ninja/compile_commands.json" '"file":"src/native.c"'
not_contains "$ninja_project/build-ninja/compile_commands.json" '"file":"src/main.zig"'
contains "$ninja_project/build-ninja/ninja/build.ninja" "depfile = build-ninja/out/___mixed/mixed.d"
contains "$ninja_project/build-ninja/ninja/build.ninja" "QSTAR_GLP_V2_ENV=alpha"

last_step=ninja-observability
"$qstar" --file "$ninja_project/qstar.lua" -B build-ninja -G ninja action-log //:mixed:link:0 > "$tmp/ninja-log.out" 2> "$tmp/ninja-log.err"
contains "$tmp/ninja-log.out" "backend=ninja"
contains "$tmp/ninja-log.out" "response_file=build-ninja/rsp/___mixed_link_0.rsp"
contains "$tmp/ninja-log.out" "env[0]=QSTAR_GLP_V2_ENV=<redacted>"
contains "$tmp/ninja-log.out" "output_count=4"
contains "$tmp/ninja-log.out" "exec_argc=2"
ninja_plan_digest=$(field_from_line "$tmp/ninja-explain.out" \
  "command_argv id=//:mixed:link:0" "digest")
ninja_log_digest=$(line_value "$tmp/ninja-log.out" "logical_argv_digest")
test "$ninja_plan_digest" = "$ninja_log_digest" ||
  fail "GLP v2 Ninja logical argv digest mismatch"
ninja_plan_rsp_digest=$(field_from_line "$tmp/ninja-explain.out" \
  "command_argv id=//:mixed:link:0" "response_digest")
ninja_log_rsp_digest=$(line_value "$tmp/ninja-log.out" "response_digest")
test "$ninja_plan_rsp_digest" = "$ninja_log_rsp_digest" ||
  fail "GLP v2 Ninja response digest mismatch"
"$qstar" --file "$ninja_project/qstar.lua" -B build-ninja -G ninja replay //:mixed:link:0 > "$tmp/ninja-replay.out" 2> "$tmp/ninja-replay.err"
contains "$tmp/ninja-replay.out" "output_count=4"
contains "$tmp/ninja-replay.out" "output[2]=build-ninja/out/___mixed/mixed.resources"
contains "$tmp/ninja-replay.out" "response_file=build-ninja/rsp/___mixed_link_0.rsp"

last_step=windows-artifacts
"$qstar" --file "$ninja_project/qstar.lua" --qstar-internal-platform windows query //:cuda_plugin --format json > "$tmp/windows-cuda.json" 2> "$tmp/windows-cuda.err"
contains "$tmp/windows-cuda.json" '"path":"build/qstar/out/___cuda_plugin/cuda_plugin.dll"'
contains "$tmp/windows-cuda.json" '"path":"build/qstar/out/___cuda_plugin/cuda_plugin.dll.import"'
contains "$tmp/windows-cuda.json" '"runtime":true,"link_interface":false'
contains "$tmp/windows-cuda.json" '"runtime":false,"link_interface":true'
"$qstar" --file "$ninja_project/qstar.lua" --qstar-internal-platform windows explain //:mixed > "$tmp/windows-mixed.out" 2> "$tmp/windows-mixed.err"
contains "$tmp/windows-mixed.out" "interface=build/qstar/out/___cuda_plugin/cuda_plugin.dll.import"
not_contains "$tmp/windows-mixed.out" "interface=build/qstar/out/___cuda_plugin/cuda_plugin.dll,"
"$qstar" --file "$ninja_project/qstar.lua" --qstar-internal-platform windows explain //:consumer > "$tmp/windows-consumer.out" 2> "$tmp/windows-consumer.err"
contains "$tmp/windows-consumer.out" "interface=build/qstar/out/___mixed/mixed.exe.import"

last_step=documentation
contains "docs/language-provider-backend-contract.md" "Q280 backend parity seal"
contains "docs/language-provider-backend-contract.md" "make qstar-glp-v2-backend-parity-tests"
contains "wiki/reference/language-providers.md" "GLP v2 Backend Parity"
contains "wiki/AI_INDEX.md" "make qstar-glp-v2-backend-parity-tests"
contains "man/man5/qstar-lua.5" "qstar-glp-v2-backend-parity-tests"

last_step=done
echo "qstar-glp-v2-backend-parity: ok"
