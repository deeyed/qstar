#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
case "$qstar" in
  /*) ;;
  *) qstar=$(pwd)/$qstar ;;
esac
fixture=tests/projects/local-action-cache
tmp=${TMPDIR:-/tmp}/qstar-local-action-cache.$$
last_step=setup

fail() {
  echo "qstar-local-action-cache: $* (step=$last_step)" >&2
  exit 1
}

contains() {
  grep -F -q -- "$2" "$1" || fail "missing '$2' in $1"
}

lacks() {
  if grep -F -q -- "$2" "$1"; then
    fail "unexpected '$2' in $1"
  fi
}

counter_is() {
  actual=0
  if test -f "$1"; then
    actual=$(cat "$1")
  fi
  test "$actual" = "$2" || fail "counter $1 is $actual, expected $2"
}

run_qstar() {
  project=$1
  shift
  PATH="$project/tools:$PATH" "$qstar" --file "$project/qstar.lua" "$@"
}

clean_target() {
  project=$1
  build_dir=$2
  generator=$3
  label=$4
  run_qstar "$project" -B "$build_dir" -G "$generator" clean "$label" >/dev/null
}

first_blob() {
  find "$1" -type f -name 'blob.0' -print | sed -n '1p'
}

cleanup() {
  rc=$?
  trap - EXIT HUP INT TERM
  if test "$rc" -ne 0; then
    find "$tmp" -type f \( -name '*.out' -o -name '*.err' \) -print 2>/dev/null |
      while IFS= read -r file; do
        echo "--- $file" >&2
        tail -n 120 "$file" >&2 || true
      done
  fi
  rm -rf "$tmp"
  exit "$rc"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmp"
cp -R "$fixture" "$tmp/stella"
cp -R "$fixture" "$tmp/ninja"
chmod +x "$tmp/stella/tools/"* "$tmp/ninja/tools/"*

last_step=graph-surface
run_qstar "$tmp/stella" list-targets --format json > "$tmp/list.json" 2> "$tmp/list.err"
contains "$tmp/list.json" '"action_cache":"local"'
contains "$tmp/list.json" '"label":"//:objects"'
contains "$tmp/list.json" '"cacheable":true'
contains "$tmp/list.json" '"label":"//:noncache_policy"'
contains "$tmp/list.json" '"has_cacheable":true,"cacheable":false'
run_qstar "$tmp/stella" query //:uncached_objects --format json > "$tmp/query.json" 2> "$tmp/query.err"
contains "$tmp/query.json" '"cacheable":false'
run_qstar "$tmp/stella" explain //:objects > "$tmp/explain.out" 2> "$tmp/explain.err"
contains "$tmp/explain.out" 'local_action_cache mode=local audit=report-only sandbox=off'
contains "$tmp/explain.out" '  cacheable true'
run_qstar "$tmp/stella" explain //:cached_artifact > "$tmp/explain-generated.out" 2> "$tmp/explain-generated.err"
contains "$tmp/explain-generated.out" 'cacheable=true'

last_step=stella-clean-build
run_qstar "$tmp/stella" -B build-stella -G stella build //:objects --progress off --explain-cache > "$tmp/stella-first.out" 2> "$tmp/stella-first.err"
counter_is "$tmp/stella/tools/compiler.count" 1
contains "$tmp/stella-first.out" 'cacheable=true reason=eligible'
contains "$tmp/stella-first.out" 'hits=0 misses=1 stores=1 corruptions=0'

last_step=stella-incremental
run_qstar "$tmp/stella" -B build-stella -G stella build //:objects --progress off --explain-cache > "$tmp/stella-incremental.out" 2> "$tmp/stella-incremental.err"
counter_is "$tmp/stella/tools/compiler.count" 1
contains "$tmp/stella-incremental.out" 'status ok run=0 skip=1 fail=0'

last_step=stella-cache-hit
clean_target "$tmp/stella" build-stella stella //:objects
run_qstar "$tmp/stella" -B build-stella -G stella build //:objects --progress off --explain-cache > "$tmp/stella-hit.out" 2> "$tmp/stella-hit.err"
counter_is "$tmp/stella/tools/compiler.count" 1
contains "$tmp/stella-hit.out" 'status=hit reason=hit'
contains "$tmp/stella-hit.out" 'hits=1 misses=0 stores=0 corruptions=0'

last_step=stella-corruption
clean_target "$tmp/stella" build-stella stella //:objects
blob=$(first_blob "$tmp/stella/build-stella/cas/v1")
test -n "$blob" || fail "Stella cache blob missing"
printf 'corrupt\n' > "$blob"
run_qstar "$tmp/stella" -B build-stella -G stella build //:objects --progress off --explain-cache > "$tmp/stella-corrupt.out" 2> "$tmp/stella-corrupt.err"
counter_is "$tmp/stella/tools/compiler.count" 2
contains "$tmp/stella-corrupt.out" 'reason=corrupt-entry'
contains "$tmp/stella-corrupt.out" 'corruptions=1'

last_step=tool-fingerprint
clean_target "$tmp/stella" build-stella stella //:objects
printf '\n# fingerprint change\n' >> "$tmp/stella/tools/fake-cc"
run_qstar "$tmp/stella" -B build-stella -G stella build //:objects --progress off --explain-cache > "$tmp/stella-tool-change.out" 2> "$tmp/stella-tool-change.err"
counter_is "$tmp/stella/tools/compiler.count" 3
contains "$tmp/stella-tool-change.out" 'hits=0 misses=1 stores=1'

last_step=stella-depfile-input
clean_target "$tmp/stella" build-stella stella //:objects
printf '\n#define QSTAR_HEADER_CHANGED 1\n' >> "$tmp/stella/src/value.h"
run_qstar "$tmp/stella" -B build-stella -G stella build //:objects --progress off --explain-cache > "$tmp/stella-header-change.out" 2> "$tmp/stella-header-change.err"
counter_is "$tmp/stella/tools/compiler.count" 4
contains "$tmp/stella-header-change.out" 'hits=0 misses=1 stores=1'

last_step=stella-generated-hit
run_qstar "$tmp/stella" -B build-stella -G stella build //:cached_artifact --progress off --explain-cache > "$tmp/stella-generated-first.out" 2> "$tmp/stella-generated-first.err"
counter_is "$tmp/stella/tools/transform.count" 1
contains "$tmp/stella-generated-first.out" 'undeclared-path=tools/transform.count report-only'
rm -f "$tmp/stella/generated/cached/payload.txt"
run_qstar "$tmp/stella" -B build-stella -G stella build //:cached_artifact --progress off --explain-cache > "$tmp/stella-generated-hit.out" 2> "$tmp/stella-generated-hit.err"
counter_is "$tmp/stella/tools/transform.count" 1
contains "$tmp/stella-generated-hit.out" 'hits=1 misses=0 stores=0'

last_step=stella-declared-noncacheable
run_qstar "$tmp/stella" -B build-stella -G stella build //:uncached_artifact --progress off --explain-cache > "$tmp/stella-uncached-first.out" 2> "$tmp/stella-uncached-first.err"
rm -f "$tmp/stella/generated/uncached/payload.txt"
run_qstar "$tmp/stella" -B build-stella -G stella build //:uncached_artifact --progress off --explain-cache > "$tmp/stella-uncached-second.out" 2> "$tmp/stella-uncached-second.err"
counter_is "$tmp/stella/tools/uncached.count" 2
contains "$tmp/stella-uncached-second.out" 'reason=declared-non-cacheable'
contains "$tmp/stella-uncached-second.out" 'eligible=0 non_cacheable=1 hits=0 misses=0 stores=0'

last_step=stella-config-noncacheable
run_qstar "$tmp/stella" -B build-stella -G stella build //:uncached_objects --progress off --explain-cache > "$tmp/stella-config-uncached-first.out" 2> "$tmp/stella-config-uncached-first.err"
clean_target "$tmp/stella" build-stella stella //:uncached_objects
run_qstar "$tmp/stella" -B build-stella -G stella build //:uncached_objects --progress off --explain-cache > "$tmp/stella-config-uncached-second.out" 2> "$tmp/stella-config-uncached-second.err"
counter_is "$tmp/stella/tools/compiler.count" 6
contains "$tmp/stella-config-uncached-second.out" 'reason=declared-non-cacheable'

last_step=stella-external-runtime
run_qstar "$tmp/stella" -B build-stella -G stella build //:external_runtime --progress off --explain-cache > "$tmp/stella-external-first.out" 2> "$tmp/stella-external-first.err"
rm -f "$tmp/stella/generated/external/probe.txt"
run_qstar "$tmp/stella" -B build-stella -G stella build //:external_runtime --progress off --explain-cache > "$tmp/stella-external-second.out" 2> "$tmp/stella-external-second.err"
counter_is "$tmp/stella/tools/external.count" 2
contains "$tmp/stella-external-second.out" 'reason=external-runtime-tool'

last_step=cli-off-override
clean_target "$tmp/stella" build-stella stella //:objects
run_qstar "$tmp/stella" -B build-stella -G stella build //:objects --progress off --action-cache off > "$tmp/cache-off-first.out" 2> "$tmp/cache-off-first.err"
clean_target "$tmp/stella" build-stella stella //:objects
run_qstar "$tmp/stella" -B build-stella -G stella build //:objects --progress off --action-cache off > "$tmp/cache-off-second.out" 2> "$tmp/cache-off-second.err"
counter_is "$tmp/stella/tools/compiler.count" 8
lacks "$tmp/cache-off-second.out" 'local_cache_stats'

last_step=ninja-clean-build
command -v ninja >/dev/null 2>&1 || fail "ninja is required"
run_qstar "$tmp/ninja" -B build-ninja -G ninja build //:objects --progress off --explain-cache > "$tmp/ninja-first.out" 2> "$tmp/ninja-first.err"
counter_is "$tmp/ninja/tools/compiler.count" 1
contains "$tmp/ninja-first.out" 'backend ninja'
contains "$tmp/ninja-first.out" 'hits=0 misses=1 stores=1 corruptions=0'

last_step=ninja-cache-hit
clean_target "$tmp/ninja" build-ninja ninja //:objects
run_qstar "$tmp/ninja" -B build-ninja -G ninja build //:objects --progress off --explain-cache > "$tmp/ninja-hit.out" 2> "$tmp/ninja-hit.err"
counter_is "$tmp/ninja/tools/compiler.count" 1
contains "$tmp/ninja-hit.out" 'status=hit reason=hit'
contains "$tmp/ninja-hit.out" 'hits=1 misses=0 stores=0 corruptions=0'
contains "$tmp/ninja/build-ninja/ninja/build.ninja" 'command = :'

last_step=ninja-corruption
clean_target "$tmp/ninja" build-ninja ninja //:objects
blob=$(first_blob "$tmp/ninja/build-ninja/cas/v1")
test -n "$blob" || fail "Ninja cache blob missing"
printf 'corrupt\n' > "$blob"
run_qstar "$tmp/ninja" -B build-ninja -G ninja build //:objects --progress off --explain-cache > "$tmp/ninja-corrupt.out" 2> "$tmp/ninja-corrupt.err"
counter_is "$tmp/ninja/tools/compiler.count" 2
contains "$tmp/ninja-corrupt.out" 'reason=corrupt-entry'
contains "$tmp/ninja-corrupt.out" 'corruptions=1'

last_step=ninja-depfile-input
clean_target "$tmp/ninja" build-ninja ninja //:objects
printf '\n#define QSTAR_HEADER_CHANGED 1\n' >> "$tmp/ninja/src/value.h"
run_qstar "$tmp/ninja" -B build-ninja -G ninja build //:objects --progress off --explain-cache > "$tmp/ninja-header-change.out" 2> "$tmp/ninja-header-change.err"
counter_is "$tmp/ninja/tools/compiler.count" 3
contains "$tmp/ninja-header-change.out" 'hits=0 misses=1 stores=1'

last_step=ninja-generated-parity
run_qstar "$tmp/ninja" -B build-ninja -G ninja build //:cached_artifact --progress off --explain-cache > "$tmp/ninja-generated-first.out" 2> "$tmp/ninja-generated-first.err"
counter_is "$tmp/ninja/tools/transform.count" 1
rm -f "$tmp/ninja/generated/cached/payload.txt"
run_qstar "$tmp/ninja" -B build-ninja -G ninja build //:cached_artifact --progress off --explain-cache > "$tmp/ninja-generated-hit.out" 2> "$tmp/ninja-generated-hit.err"
counter_is "$tmp/ninja/tools/transform.count" 1
contains "$tmp/ninja-generated-hit.out" 'hits=1 misses=0 stores=0'

last_step=diagnostics
mkdir -p "$tmp/bad"
cat > "$tmp/bad/qstar.lua" <<'EOF'
qstar.project {
  name = "bad-cacheability",
  action_cache = "remote",
}
EOF
if "$qstar" --file "$tmp/bad/qstar.lua" check > "$tmp/bad-project.out" 2> "$tmp/bad-project.err"; then
  fail "unsupported project action_cache unexpectedly succeeded"
fi
contains "$tmp/bad-project.err" 'action_cache must be "off" or "local"'

cat > "$tmp/bad/qstar.lua" <<'EOF'
qstar.objectlib "bad" {
  cacheable = "sometimes",
  sources = {"input.c"},
}
EOF
if "$qstar" --file "$tmp/bad/qstar.lua" check > "$tmp/bad-cacheable.out" 2> "$tmp/bad-cacheable.err"; then
  fail "non-boolean cacheable unexpectedly succeeded"
fi
contains "$tmp/bad-cacheable.err" "field 'cacheable' must be boolean"

if "$qstar" --action-cache remote --file "$tmp/stella/qstar.lua" check > "$tmp/bad-cli.out" 2> "$tmp/bad-cli.err"; then
  fail "unsupported CLI action cache mode unexpectedly succeeded"
fi
contains "$tmp/bad-cli.err" "invalid action cache mode 'remote'"

echo "qstar local action cache: ok"
