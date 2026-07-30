#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
qstar=${QSTAR_TEST_QSTAR:-"$root/build/bin/qstar"}
fixture=$root/tests/projects/wide-final-action
timer=$root/tests/timed-command.py
counts=${QSTAR_LARGE_FINAL_OBJECTS:-"1000 4096"}
report_only=${QSTAR_LARGE_FINAL_REPORT_ONLY:-1}
tmp=${TMPDIR:-/tmp}/qstar-wide-final-performance-$$
issues=0

fail() {
  printf 'wide_final_gate: %s\n' "$*" >&2
  exit 1
}

cleanup() {
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

timed() {
  name=$1
  shift
  record=$(python3 "$timer" "$tmp/$name.out" "$tmp/$name.err" -- "$@") || {
    cat "$tmp/$name.out" >&2
    cat "$tmp/$name.err" >&2
    fail "phase $name failed"
  }
  elapsed=$(printf '%s\n' "$record" |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["elapsed_ms"])')
  peak=$(printf '%s\n' "$record" |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["peak_kb"])')
}

issue() {
  issues=$((issues + 1))
  if test "$report_only" = 1; then
    printf 'wide_final_gate warning=%s\n' "$*"
  else
    fail "$*"
  fi
}

rm -rf "$tmp"
mkdir -p "$tmp"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"
command -v ninja >/dev/null 2>&1 || fail "ninja is required"

baseline_graph=
baseline_lower=
baseline_emit=
for count in $counts; do
  case "$count" in
    ''|*[!0-9]*) fail "invalid object count '$count'" ;;
  esac
  test "$count" -ge 1000 || fail "object count must be at least 1000"
  project=$tmp/project-$count
  mkdir -p "$project"
  cp -R "$fixture"/. "$project"/
  chmod +x "$project"/tools/*
  mkdir -p "$project/objects/prebuilt" "$project/objects/imported" \
    "$project/objects/quoted" "$project/src/direct"
  i=0
  while test "$i" -lt "$count"; do
    printf 'wide performance object %s\n' "$i" \
      > "$project/objects/prebuilt/object-$(printf '%04d' "$i").o"
    i=$((i + 1))
  done
  printf 'imported\n' > "$project/objects/imported/imported-object.o"
  printf 'quoted\n' > "$project/objects/quoted/object with space.o"
  i=0
  while test "$i" -lt 256; do
    printf 'int wide_unit_%s(void) { return %s; }\n' "$i" "$i" \
      > "$project/src/direct/unit-$(printf '%04d' "$i").c"
    i=$((i + 1))
  done

  timed "graph-$count" "$qstar" --file "$project/qstar.lua" \
    -D final-objects="$count" -D direct-sources=256 \
    -B build/stella check //:multiple_objectlibs
  graph_ms=$elapsed
  printf 'wide_final_gate objects=%s phase=graph-evaluation elapsed_ms=%s peak_kb=%s\n' \
    "$count" "$elapsed" "$peak"

  timed "lower-$count" "$qstar" --file "$project/qstar.lua" \
    -D final-objects="$count" -D direct-sources=256 \
    -B build/stella dry-run //:multiple_objectlibs
  lower_ms=$elapsed
  grep -F -q "object_count=$count" "$tmp/lower-$count.out" ||
    fail "dry-run omitted object_count=$count"
  grep -F -q "response=skeleton" "$tmp/lower-$count.out" ||
    fail "dry-run did not predict a response file"
  digest=$(sed -n 's/.* response_digest=\([^ ]*\).*/\1/p' \
    "$tmp/lower-$count.out" | tail -n 1)
  test -n "$digest" || fail "dry-run response digest missing"
  printf 'wide_final_gate objects=%s phase=logical-argv-lowering elapsed_ms=%s peak_kb=%s response_digest=%s\n' \
    "$count" "$elapsed" "$peak" "$digest"

  timed "emit-$count" "$qstar" --file "$project/qstar.lua" \
    -D final-objects="$count" -D direct-sources=256 \
    -B build/ninja -G ninja emit-ninja //:multiple_objectlibs
  emit_ms=$elapsed
  grep -F -q "@build/ninja/rsp/___multiple_objectlibs_link_0.rsp" \
    "$project/build/ninja/ninja/build.ninja" ||
    fail "Ninja emission omitted the response file"
  printf 'wide_final_gate objects=%s phase=ninja-emission elapsed_ms=%s peak_kb=%s\n' \
    "$count" "$elapsed" "$peak"

  timed "stella-clean-$count" "$qstar" --file "$project/qstar.lua" \
    -D final-objects="$count" -D direct-sources=256 \
    -B build/stella --progress off build //:multiple_objectlibs
  grep -F -q "status ok" "$tmp/stella-clean-$count.out"
  printf 'wide_final_gate objects=%s backend=stella phase=clean elapsed_ms=%s peak_kb=%s\n' \
    "$count" "$elapsed" "$peak"
  rsp=$project/build/stella/rsp/___multiple_objectlibs_link_0.rsp
  test -f "$rsp" || fail "Stella response file missing"
  rsp_bytes=$(wc -c < "$rsp" | tr -d ' ')
  rsp_lines=$(wc -l < "$rsp" | tr -d ' ')
  test "$rsp_lines" -eq $((count + 4)) ||
    fail "response atom count mismatch for $count objects"
  sed -n '5p' "$rsp" | grep -F -q "objects/prebuilt/object-0000.o" ||
    fail "response file first object changed"
  tail -n 1 "$rsp" |
    grep -F -q "objects/prebuilt/object-$(printf '%04d' $((count - 1))).o" ||
    fail "response file last object changed"
  "$root/tests/wide-final-verify.py" \
    "$project/build/stella/out/___multiple_objectlibs/multiple_objectlibs.wide.json" \
    --count "$count" --style posix --response yes \
    --first objects/prebuilt/object-0000.o \
    --last "objects/prebuilt/object-$(printf '%04d' $((count - 1))).o" \
    > "$tmp/stella-record-$count.out"
  printf 'wide_final_gate objects=%s phase=response-write bytes=%s atoms=%s response_digest=%s\n' \
    "$count" "$rsp_bytes" "$rsp_lines" "$digest"

  timed "stella-noop-$count" "$qstar" --file "$project/qstar.lua" \
    -D final-objects="$count" -D direct-sources=256 \
    -B build/stella --progress off build //:multiple_objectlibs
  printf 'wide_final_gate objects=%s backend=stella phase=noop elapsed_ms=%s peak_kb=%s\n' \
    "$count" "$elapsed" "$peak"

  timed "ninja-clean-$count" "$qstar" --file "$project/qstar.lua" \
    -D final-objects="$count" -D direct-sources=256 \
    -B build/ninja -G ninja --progress off build //:multiple_objectlibs
  grep -F -q "status ok" "$tmp/ninja-clean-$count.out"
  "$root/tests/wide-final-verify.py" \
    "$project/build/ninja/out/___multiple_objectlibs/multiple_objectlibs.wide.json" \
    --count "$count" --style posix --response yes \
    --first objects/prebuilt/object-0000.o \
    --last "objects/prebuilt/object-$(printf '%04d' $((count - 1))).o" \
    > "$tmp/ninja-record-$count.out"
  printf 'wide_final_gate objects=%s backend=ninja phase=clean elapsed_ms=%s peak_kb=%s\n' \
    "$count" "$elapsed" "$peak"

  timed "ninja-noop-$count" "$qstar" --file "$project/qstar.lua" \
    -D final-objects="$count" -D direct-sources=256 \
    -B build/ninja -G ninja --progress off build //:multiple_objectlibs
  printf 'wide_final_gate objects=%s backend=ninja phase=noop elapsed_ms=%s peak_kb=%s\n' \
    "$count" "$elapsed" "$peak"

  if test -z "$baseline_graph"; then
    baseline_graph=$graph_ms
    baseline_lower=$lower_ms
    baseline_emit=$emit_ms
  elif test "$count" -ge 4096; then
    test "$graph_ms" -le $((baseline_graph * 8 + 500)) ||
      issue "phase=graph-evaluation objects=$count suggests superlinear growth"
    test "$lower_ms" -le $((baseline_lower * 8 + 500)) ||
      issue "phase=logical-argv-lowering objects=$count suggests superlinear growth"
    test "$emit_ms" -le $((baseline_emit * 8 + 500)) ||
      issue "phase=ninja-emission objects=$count suggests superlinear growth"
  fi
done

printf 'wide_final_gate status=ok counts="%s" perf_issue_count=%s report_only=%s\n' \
  "$counts" "$issues" "$report_only"
