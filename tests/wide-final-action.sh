#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
qstar=${QSTAR_TEST_QSTAR:-"$root/build/bin/qstar"}
cc=${CC:-cc}
final_objects=${QSTAR_WIDE_FINAL_OBJECTS:-1000}
direct_sources=${QSTAR_WIDE_FINAL_DIRECT_SOURCES:-256}
tmp=${TMPDIR:-/tmp}/qstar-wide-final-action-$$
project=$tmp/project
fixture=$root/tests/projects/wide-final-action

fail() {
  printf 'qstar-wide-final-action: %s\n' "$*" >&2
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

field_from_plan() {
  file=$1
  action=$2
  field=$3
  awk -v action="$action" -v field="$field" '
    index($0, "command_argv id=" action " ") {
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

field_from_log() {
  file=$1
  field=$2
  sed -n "s/^${field}=//p" "$file" | tail -n 1
}

cleanup() {
  rc=$?
  if test "$rc" -ne 0; then
    for file in "$tmp"/*.err; do
      test -f "$file" && {
        printf '%s\n' "--- $file" >&2
        tail -n 120 "$file" >&2
      }
    done
  fi
  if test "${QSTAR_KEEP_TEST_TMP:-0}" = 1; then
    printf 'qstar-wide-final-action: preserved=%s\n' "$tmp" >&2
  else
    rm -rf "$tmp"
  fi
}
trap cleanup EXIT HUP INT TERM

case "$final_objects:$direct_sources" in
  *[!0-9:]*|:*)
    fail "object and source counts must be non-negative integers"
    ;;
esac
if test "$final_objects" -lt 1000; then
  fail "QSTAR_WIDE_FINAL_OBJECTS must be at least 1000"
fi
if test "$direct_sources" -lt 256; then
  fail "QSTAR_WIDE_FINAL_DIRECT_SOURCES must be at least 256"
fi
command -v python3 >/dev/null 2>&1 ||
  fail "python3 is required by the fake response-file tools"
command -v ninja >/dev/null 2>&1 ||
  fail "ninja is required by the wide final backend parity gate"

rm -rf "$tmp"
mkdir -p "$project"
cp -R "$fixture"/. "$project"/
chmod +x "$project"/tools/*

mkdir -p "$project/objects/prebuilt" "$project/objects/imported" \
  "$project/objects/quoted" "$project/src/direct"
i=0
while test "$i" -lt "$final_objects"; do
  printf 'prebuilt object %s\n' "$i" \
    > "$project/objects/prebuilt/object-$(printf '%04d' "$i").o"
  i=$((i + 1))
done
printf 'imported object\n' > "$project/objects/imported/imported-object.o"
printf 'quoted object\n' > "$project/objects/quoted/object with space.o"
i=0
while test "$i" -lt "$direct_sources"; do
  printf 'int wide_unit_%s(void) { return %s; }\n' "$i" "$i" \
    > "$project/src/direct/unit-$(printf '%04d' "$i").c"
  i=$((i + 1))
done

"$cc" -std=c99 -Wall -Wextra -Wpedantic -Werror \
  -I"$root/include" -I"$root/src" \
  "$root/tests/argv-failure-unit.c" -o "$tmp/argv-failure-unit"
"$tmp/argv-failure-unit" > "$tmp/argv-failure-unit.out"
contains "$tmp/argv-failure-unit.out" "qstar-argv-failure-unit: passed"

if grep -R -n 'QSTAR_EXEC_MAX_ARGV' "$root/include" "$root/src" \
  > "$tmp/drift-exec.out" 2>&1; then
  cat "$tmp/drift-exec.out" >&2
  fail "QSTAR_EXEC_MAX_ARGV returned to command-capacity code"
fi
if grep -R -n 'QSTAR_NINJA_MAX_ARGV' "$root/include" "$root/src" \
  > "$tmp/drift-ninja.out" 2>&1; then
  cat "$tmp/drift-ninja.out" >&2
  fail "QSTAR_NINJA_MAX_ARGV returned to command-capacity code"
fi
if grep -R -n 'has too many object inputs' "$root/include" "$root/src" \
  > "$tmp/drift-diagnostic.out" 2>&1; then
  cat "$tmp/drift-diagnostic.out" >&2
  fail "legacy object-input diagnostic returned"
fi

(
  cd "$project"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" check //... \
    > "$tmp/check.out" 2> "$tmp/check.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/stella dry-run //:cardinality_1000 \
    > "$tmp/stella-dry.out" 2> "$tmp/stella-dry.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/stella explain //:cardinality_1000 \
    > "$tmp/stella-explain.out" 2> "$tmp/stella-explain.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/ninja -G ninja dry-run //:cardinality_1000 \
    > "$tmp/ninja-dry.out" 2> "$tmp/ninja-dry.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/ninja -G ninja explain //:cardinality_1000 \
    > "$tmp/ninja-explain.out" 2> "$tmp/ninja-explain.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/parity dry-run //:cardinality_1000 \
    > "$tmp/parity-stella-dry.out" 2> "$tmp/parity-stella-dry.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/parity -G ninja dry-run //:cardinality_1000 \
    > "$tmp/parity-ninja-dry.out" 2> "$tmp/parity-ninja-dry.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/stella build //:fast_matrix --progress off --schedule-trace \
    > "$tmp/stella-build.out" 2> "$tmp/stella-build.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/stella build //:fast_matrix --progress off --schedule-trace \
    > "$tmp/stella-noop.out" 2> "$tmp/stella-noop.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/ninja -G ninja build //:fast_matrix --progress off \
    > "$tmp/ninja-build.out" 2> "$tmp/ninja-build.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/stella --progress off test //:wide_test \
    > "$tmp/stella-test.out" 2> "$tmp/stella-test.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/ninja -G ninja --progress off test //:wide_test \
    > "$tmp/ninja-test.out" 2> "$tmp/ninja-test.err"

  for backend in stella ninja; do
    generator=
    test "$backend" = ninja && generator="-G ninja"
    # shellcheck disable=SC2086
    "$qstar" -D final-objects="$final_objects" \
      -D direct-sources="$direct_sources" \
      -B "build/$backend" $generator \
      action-log //:cardinality_1000:link:0 \
      > "$tmp/$backend-log.out" 2> "$tmp/$backend-log.err"
    # shellcheck disable=SC2086
    "$qstar" -D final-objects="$final_objects" \
      -D direct-sources="$direct_sources" \
      -B "build/$backend" $generator \
      replay //:cardinality_1000:link:0 \
      > "$tmp/$backend-replay.out" 2> "$tmp/$backend-replay.err"
  done

  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/stella dry-run //:logical_argc_48 \
    > "$tmp/logical-48-dry.out" 2> "$tmp/logical-48-dry.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/stella dry-run //:logical_argc_49 \
    > "$tmp/logical-49-dry.out" 2> "$tmp/logical-49-dry.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/stella action-log //:logical_argc_48:generate:0 \
    > "$tmp/logical-48-log.out" 2> "$tmp/logical-48-log.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/stella action-log //:logical_argc_49:generate:0 \
    > "$tmp/logical-49-log.out" 2> "$tmp/logical-49-log.err"

  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/cas build //:direct_sources --progress off --explain-cache \
    > "$tmp/cas.out" 2> "$tmp/cas.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/ninja -G ninja emit-ninja //:cardinality_1000 \
    > "$tmp/ninja-emit.out" 2> "$tmp/ninja-emit.err"
)

contains "$tmp/check.out" "status ok"
contains "$tmp/stella-build.out" "status ok"
contains "$tmp/stella-noop.out" "status ok"
contains "$tmp/ninja-build.out" "backend ninja"
contains "$tmp/ninja-build.out" "status ok"
contains "$tmp/stella-test.out" "test_result label=//:wide_test status=pass"
contains "$tmp/ninja-test.out" "test_result label=//:wide_test status=pass"
contains "$tmp/logical-48-dry.out" "logical_argc=48"
contains "$tmp/logical-48-dry.out" "response=none"
contains "$tmp/logical-49-dry.out" "logical_argc=49"
contains "$tmp/logical-49-dry.out" "response=skeleton"
contains "$tmp/logical-48-log.out" "exec_argc=48"
contains "$tmp/logical-49-log.out" "exec_argc=2"
contains "$tmp/cas.out" "local_cache_stats"
contains "$tmp/cas.out" "reason=action-kind-not-supported"

action=//:cardinality_1000:link:0
for backend in stella ninja; do
  dry=$tmp/$backend-dry.out
  explain=$tmp/$backend-explain.out
  log=$tmp/$backend-log.out
  contains "$dry" "object_count=1000"
  contains "$dry" "input_count=1000"
  contains "$dry" "toolset=//:response_on_tools"
  contains "$dry" "output=build/$backend/out/___cardinality_1000/cardinality_1000"
  contains "$dry" "response=skeleton"
  contains "$dry" "exec_argc=2"
  contains "$log" "output[0]=build/$backend/out/___cardinality_1000/cardinality_1000"
  contains "$log" "response_style=posix"
  contains "$log" "exec_argc=2"
  dry_argc=$(field_from_plan "$dry" "$action" logical_argc)
  dry_digest=$(field_from_plan "$dry" "$action" digest)
  dry_rsp_digest=$(field_from_plan "$dry" "$action" response_digest)
  dry_rsp=$(field_from_plan "$dry" "$action" response_file)
  explain_argc=$(field_from_plan "$explain" "$action" logical_argc)
  explain_digest=$(field_from_plan "$explain" "$action" digest)
  log_argc=$(field_from_log "$log" logical_argc)
  log_digest=$(field_from_log "$log" logical_argv_digest)
  log_rsp_digest=$(field_from_log "$log" response_digest)
  log_rsp=$(field_from_log "$log" response_file)
  test "$dry_argc" = "$explain_argc" ||
    fail "$backend dry-run/explain argc mismatch"
  test "$dry_digest" = "$explain_digest" ||
    fail "$backend dry-run/explain digest mismatch"
  test "$dry_argc" = "$log_argc" ||
    fail "$backend dry-run/action-log argc mismatch"
  test "$dry_digest" = "$log_digest" ||
    fail "$backend dry-run/action-log argv digest mismatch"
  test "$dry_rsp_digest" = "$log_rsp_digest" ||
    fail "$backend dry-run/action-log response digest mismatch"
  test "$dry_rsp" = "$log_rsp" ||
    fail "$backend dry-run/action-log response path mismatch"
  contains "$tmp/$backend-replay.out" "objects/prebuilt/object-0999.o"
  test -f "$project/$log_rsp" ||
    fail "$backend response file is missing"
done

for field in logical_argc digest response_digest response_file object_count \
  input_count exec_argc; do
  stella_value=$(field_from_plan "$tmp/parity-stella-dry.out" "$action" "$field")
  ninja_value=$(field_from_plan "$tmp/parity-ninja-dry.out" "$action" "$field")
  test "$stella_value" = "$ninja_value" ||
    fail "Stella/Ninja same-root $field values differ"
done

verify=$root/tests/wide-final-verify.py
for backend in stella ninja; do
  "$verify" "$project/build/$backend/out/___cardinality_0/cardinality_0.wide.json" \
    --count 0 --style posix
  "$verify" "$project/build/$backend/out/___cardinality_1/cardinality_1.wide.json" \
    --count 1 --style posix --first objects/prebuilt/object-0000.o
  for count in 48 49 252 253 256; do
    "$verify" \
      "$project/build/$backend/out/___cardinality_$count/cardinality_$count.wide.json" \
      --count "$count" --style posix
  done
  "$verify" \
    "$project/build/$backend/out/___cardinality_1000/cardinality_1000.wide.json" \
    --count 1000 --style posix --response yes \
    --first objects/prebuilt/object-0000.o \
    --last objects/prebuilt/object-0999.o
  "$verify" \
    "$project/build/$backend/out/___wide_static/libwide_static.a.wide.json" \
    --count "$final_objects" --style posix --response yes
  shared_record=$(find "$project/build/$backend/out/___wide_shared" \
    -name '*.wide.json' -type f -print -quit)
  test -n "$shared_record" || fail "$backend sharedlib record missing"
  "$verify" "$shared_record" --count "$final_objects" --style posix --response yes
  "$verify" \
    "$project/build/$backend/out/___direct_sources/direct_sources.wide.json" \
    --count "$direct_sources" --style posix --response yes
  "$verify" \
    "$project/build/$backend/out/___own_context_consumer/own_context_consumer.wide.json" \
    --count 48 --style posix
  "$verify" \
    "$project/build/$backend/out/___consumer_context_consumer/consumer_context_consumer.wide.json" \
    --count 49 --style posix --response yes
  "$verify" \
    "$project/build/$backend/out/___multiple_objectlibs/multiple_objectlibs.wide.json" \
    --count "$final_objects" --style posix --response yes \
    --first objects/prebuilt/object-0000.o \
    --last "objects/prebuilt/object-$(printf '%04d' $((final_objects - 1))).o"
  "$verify" \
    "$project/build/$backend/out/___generated_bridge/generated_bridge.wide.json" \
    --count 49 --style posix --response yes
  "$verify" \
    "$project/build/$backend/out/___imported_bridge/imported_bridge.wide.json" \
    --count 1 --style posix --first objects/imported/imported-object.o
  "$verify" \
    "$project/build/$backend/out/___response_auto/response_auto.wide.json" \
    --count 253 --style posix --response yes
  "$verify" \
    "$project/build/$backend/out/___response_off/response_off.wide.json" \
    --count 256 --style posix --response no
  "$verify" \
    "$project/build/$backend/out/___response_msvc/response_msvc.wide.json" \
    --count 1 --style msvc --response yes \
    --first "objects/quoted/object with space.o"
  "$verify" \
    "$project/build/$backend/out/___provider_v1_wide/provider_v1_wide.wide.json" \
    --count 0 --style posix --response yes
  "$verify" \
    "$project/build/$backend/out/___provider_v2_mixed/provider_v2_mixed.wide.json" \
    --count "$final_objects" --style posix --response yes
done

contains "$project/build/ninja/ninja/build.ninja" \
  "@build/ninja/rsp/___cardinality_1000_link_0.rsp"
contains "$project/build/stella/rsp/___response_msvc_link_0.rsp" \
  '"option with space"'
contains "$project/build/stella/rsp/___response_msvc_link_0.rsp" \
  '"C:\wide path\tail\\"'
contains "$project/build/stella/rsp/___response_msvc_link_0.rsp" \
  '"quote\"inside"'

stella_rsp=$project/$(field_from_log "$tmp/stella-log.out" response_file)
printf 'truncated-corrupt-response\n' > "$stella_rsp"
printf 'changed prebuilt object\n' \
  > "$project/objects/prebuilt/object-0500.o"
(
  cd "$project"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/stella why-rebuild //:cardinality_1000 \
    > "$tmp/why-input.out" 2> "$tmp/why-input.err"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/stella build //:cardinality_1000 --progress off \
    > "$tmp/rebuild-after-rsp-corrupt.out" \
    2> "$tmp/rebuild-after-rsp-corrupt.err"
)
contains "$tmp/why-input.out" "reason=input-changed"
not_contains "$stella_rsp" "truncated-corrupt-response"
"$verify" \
  "$project/build/stella/out/___cardinality_1000/cardinality_1000.wide.json" \
  --count 1000 --style posix --response yes

: > "$project/build/stella/state/state.db"
(
  cd "$project"
  "$qstar" -D final-objects="$final_objects" \
    -D direct-sources="$direct_sources" \
    -B build/stella build //:cardinality_1000 --progress off --schedule-trace \
    > "$tmp/malformed-state.out" 2> "$tmp/malformed-state.err"
)
contains "$tmp/malformed-state.out" \
  "dirty_state_db status=miss reason=stale-or-invalid"
contains "$tmp/malformed-state.out" "status ok"

printf 'qstar-wide-final-action: status=ok final_objects=%s direct_sources=%s backends=stella,ninja\n' \
  "$final_objects" "$direct_sources"
