#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
case "$qstar" in
  /*) ;;
  *) qstar=$(pwd)/$qstar ;;
esac
tmp=${TMPDIR:-/tmp}/qstar-glp-v2-artifact-contract.$$
fixture=tests/projects/glp-v2-artifact-contract

cleanup() {
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "qstar-glp-v2-artifact-contract: failed (exit $rc)" >&2
    find "$tmp" -type f \( -name '*.out' -o -name '*.err' \) -print 2>/dev/null |
      while IFS= read -r file; do
        echo "--- $file" >&2
        tail -n 80 "$file" >&2 || true
      done
  fi
  rm -rf "$tmp"
  exit "$rc"
}

contains() {
  file=$1
  pattern=$2
  grep -F -q -- "$pattern" "$file" || {
    echo "qstar-glp-v2-artifact-contract: missing '$pattern' in $file" >&2
    return 1
  }
}

expect_failure() {
  prefix=$1
  shift
  if "$@" > "$tmp/$prefix.out" 2> "$tmp/$prefix.err"; then
    echo "qstar-glp-v2-artifact-contract: expected failure: $prefix" >&2
    return 1
  fi
}

trap cleanup EXIT HUP INT TERM
mkdir -p "$tmp"

project=$tmp/project
cp -R "$fixture" "$project"
chmod +x "$project/tools/fake-pack.sh" "$project/tools/inspect-pack.sh"

"$qstar" --file "$project/qstar.lua" check //... > "$tmp/check.out" 2> "$tmp/check.err"
"$qstar" --file "$project/qstar.lua" --dump-graph > "$tmp/graph.out" 2> "$tmp/graph.err"
contains "$tmp/graph.out" "language_provider namespace=zig id=zig api=qstar.lang/1"
contains "$tmp/graph.out" "language_provider namespace=legacy id=legacy api=qstar.lang/1"
contains "$tmp/graph.out" "language_provider namespace=pack id=pack api=qstar.lang/2"
contains "$tmp/graph.out" "final executable lower=link_executable inputs=[sources, objects, link_interfaces, link_inputs, link_options]"
contains "$tmp/graph.out" "artifact id=resources role=runtime"
contains "$tmp/graph.out" "type=tree"
contains "$tmp/graph.out" "link_interface=true"
contains "$tmp/graph.out" "provider_final api=qstar.lang/2 provider=pack"
QSTAR_PROVIDER_DIR="$project/qstar/languages" \
  "$qstar" init app "$tmp/v2-init" --use-language=pack --dry-run \
  > "$tmp/v2-init.out" 2> "$tmp/v2-init.err"
contains "$tmp/v2-init.out" "language pack"
contains "$tmp/v2-init.out" "status ok"

"$qstar" --file "$project/qstar.lua" query //:mixed --format json > "$tmp/query.out" 2> "$tmp/query.err"
contains "$tmp/query.out" '"type":"tree"'
contains "$tmp/query.out" '"runtime":true'
contains "$tmp/query.out" '"link_interface":true'
contains "$tmp/query.out" '"provider_final":{"api":"qstar.lang/2"'
contains "$tmp/query.out" '"build/qstar/out/___mixed/obj0.o"'
contains "$tmp/query.out" '"vendor/prebuilt.link"'

"$qstar" --file "$project/qstar.lua" explain //:mixed > "$tmp/explain.out" 2> "$tmp/explain.err"
contains "$tmp/explain.out" "action compile source=src/native.c"
contains "$tmp/explain.out" "tools/fake-pack.sh, final"
"$qstar" --file "$project/qstar.lua" dry-run //:mixed > "$tmp/dry-run.out" 2> "$tmp/dry-run.err"
contains "$tmp/dry-run.out" "kind=compile language=c"
contains "$tmp/dry-run.out" "tool=pack"

"$qstar" --file "$project/qstar.lua" build //:all > "$tmp/stella.out" 2> "$tmp/stella.err"
test -f "$project/build/qstar/out/___mixed/mixed"
test -f "$project/build/qstar/out/___mixed/mixed.metadata"
test -f "$project/build/qstar/out/___mixed/mixed.resources/index.txt"
test -f "$project/build/qstar/out/___mixed/mixed.link"
test -f "$project/build/qstar/out/___consumer/consumer.metadata"
test -f "$project/generated/inspected.txt"
contains "$project/build/qstar/out/___mixed/mixed.metadata" "source=src/main.p2"
contains "$project/build/qstar/out/___mixed/mixed.metadata" "object=build/qstar/out/___mixed/obj0.o"
contains "$project/build/qstar/out/___mixed/mixed.metadata" "object=build/qstar/out/___mixed/obj2.o"
contains "$project/build/qstar/out/___mixed/mixed.metadata" "interface=vendor/prebuilt.link"
contains "$project/build/qstar/out/___mixed/mixed.metadata" "link-input=vendor/extra.input"
contains "$project/build/qstar/out/___mixed/mixed.metadata" "link-option=--explicit-provider-option"
contains "$project/build/qstar/out/___consumer/consumer.metadata" "interface=build/qstar/out/___mixed/mixed.link"
"$qstar" --file "$project/qstar.lua" build //:all > "$tmp/stella-repeat.out" 2> "$tmp/stella-repeat.err"
contains "$tmp/stella-repeat.out" "status ok run=0 skip="
"$qstar" --file "$project/qstar.lua" action-log //:mixed:link:0 > "$tmp/action-log.out" 2> "$tmp/action-log.err"
contains "$tmp/action-log.out" "tools/fake-pack.sh"
"$qstar" --file "$project/qstar.lua" replay //:mixed:link:0 > "$tmp/replay.out" 2> "$tmp/replay.err"
contains "$tmp/replay.out" "mixed.resources"

rm -rf "$project/generated"
"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja build //:all > "$tmp/ninja.out" 2> "$tmp/ninja.err"
test -f "$project/build-ninja/out/___mixed/mixed"
test -f "$project/build-ninja/out/___mixed/mixed.metadata"
test -f "$project/build-ninja/out/___mixed/mixed.resources/index.txt"
test -f "$project/build-ninja/out/___mixed/mixed.link"
test -f "$project/build-ninja/out/___consumer/consumer.metadata"
test -f "$project/generated/inspected.txt"
contains "$project/build-ninja/ninja/build.ninja" "build-ninja/out/___mixed/obj0.o"
contains "$project/build-ninja/ninja/build.ninja" "vendor/prebuilt.link"
"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja action-log //:mixed:link:0 > "$tmp/ninja-log.out" 2> "$tmp/ninja-log.err"
contains "$tmp/ninja-log.out" "backend=ninja"

collision=$tmp/collision
cp -R "$fixture" "$collision"
cp -R "$collision/qstar/languages/pack" "$collision/qstar/languages/other"
sed 's/pack/other/g; s/\.p2/.o2/g' "$collision/qstar/languages/other/pack.qsm" > "$collision/qstar/languages/other/other.qsm"
sed 's/pack/other/g' "$collision/qstar/languages/other/provider.lua" > "$collision/qstar/languages/other/provider.tmp"
mv "$collision/qstar/languages/other/provider.tmp" "$collision/qstar/languages/other/provider.lua"
rm "$collision/qstar/languages/other/pack.qsm"
printf 'int other_source(void) { return 1; }\n' > "$collision/src/other.o2"
cat > "$collision/qstar.lua" <<'EOF'
local pack = qstar.use_language("qstar/languages/pack")
local other = qstar.use_language("qstar/languages/other")
qstar.project {name = "collision", root = ".", build_dir = "build/qstar"}
qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    pack = pack.tools {compiler = qstar.cli {"tools/fake-pack.sh"}},
    other = other.tools {compiler = qstar.cli {"tools/fake-pack.sh"}},
  },
}
qstar.executable "ambiguous" {
  toolset = "//:host",
  sources = {"src/main.p2", "src/other.o2"},
}
EOF
expect_failure collision "$qstar" --file "$collision/qstar.lua" check
contains "$tmp/collision.err" "multiple qstar.lang/2 final owners 'pack' and 'other'"

make_bad_manifest() {
  name=$1
  body=$2
  dir=$tmp/$name
  mkdir -p "$dir/qstar/languages/bad"
  printf '%s\n' 'return {}' > "$dir/qstar/languages/bad/provider.lua"
  printf '%s\n' "$body" > "$dir/qstar/languages/bad/bad.qsm"
  cat > "$dir/qstar.lua" <<'EOF'
qstar.use_language("qstar/languages/bad")
qstar.project {name = "bad", root = ".", build_dir = "build/qstar"}
EOF
  expect_failure "$name" "$qstar" --file "$dir/qstar.lua" check
}

make_bad_manifest unknown-version 'return qstar.language_provider {api="qstar.lang/99",id="bad",version="1",namespace="bad",implementation="provider.lua",tools={},units={},options={},exports={x="x"}}'
contains "$tmp/unknown-version.err" "supported APIs: qstar.lang/1, qstar.lang/2"
if QSTAR_PROVIDER_DIR="$tmp/unknown-version/qstar/languages" \
  "$qstar" init app "$tmp/unknown-init" --use-language=bad \
  > "$tmp/unknown-init.out" 2> "$tmp/unknown-init.err"; then
  echo "qstar-glp-v2-artifact-contract: expected init API negotiation failure" >&2
  exit 1
fi
contains "$tmp/unknown-init.err" "supported APIs: qstar.lang/1, qstar.lang/2"

make_bad_manifest duplicate-primary 'return qstar.language_provider {api="qstar.lang/2",id="bad",version="1",namespace="bad",implementation="provider.lua",tools={},units={},finals={executable={lower="link",inputs={},artifacts={one={type="file",roles={"primary"}},two={type="file",roles={"primary"}}}}},options={},exports={x="x"}}'
contains "$tmp/duplicate-primary.err" "requires exactly one primary role"

make_bad_manifest tree-link 'return qstar.language_provider {api="qstar.lang/2",id="bad",version="1",namespace="bad",implementation="provider.lua",tools={},units={},finals={executable={lower="link",inputs={},artifacts={runtime={type="file",roles={"primary"}},tree={type="tree",roles={"secondary","link-interface"}}}}},options={},exports={x="x"}}'
contains "$tmp/tree-link.err" "cannot be a link-interface"

make_bad_manifest v1-v2-field 'return qstar.language_provider {api="qstar.lang/1",id="bad",version="1",namespace="bad",implementation="provider.lua",tools={},units={},finals={executable={lower="link",inputs={"sources"}}},options={},exports={x="x"}}'
contains "$tmp/v1-v2-field.err" "unknown language provider final field finals.executable.inputs"

missing_output=$tmp/missing-output
cp -R "$fixture" "$missing_output"
sed '/^      ctx.output("metadata"),$/d' "$missing_output/qstar/languages/pack/provider.lua" > "$missing_output/qstar/languages/pack/provider.tmp"
mv "$missing_output/qstar/languages/pack/provider.tmp" "$missing_output/qstar/languages/pack/provider.lua"
expect_failure missing-output "$qstar" --file "$missing_output/qstar.lua" check
contains "$tmp/missing-output.err" 'outputs must include ctx.output("metadata")'

missing_input=$tmp/missing-input
cp -R "$fixture" "$missing_input"
sed 's/^    inputs = inputs,$/    inputs = {},/' "$missing_input/qstar/languages/pack/provider.lua" > "$missing_input/qstar/languages/pack/provider.tmp"
mv "$missing_input/qstar/languages/pack/provider.tmp" "$missing_input/qstar/languages/pack/provider.lua"
expect_failure missing-input "$qstar" --file "$missing_input/qstar.lua" check
contains "$tmp/missing-input.err" 'inputs must include ctx.input("sources") item'

unowned_output=$tmp/unowned-output
cp -R "$fixture" "$unowned_output"
sed '/^      ctx.output("runtime"),$/a\
      "generated/unowned.out",' "$unowned_output/qstar/languages/pack/provider.lua" > "$unowned_output/qstar/languages/pack/provider.tmp"
mv "$unowned_output/qstar/languages/pack/provider.tmp" "$unowned_output/qstar/languages/pack/provider.lua"
expect_failure unowned-output "$qstar" --file "$unowned_output/qstar.lua" check
contains "$tmp/unowned-output.err" "is not owned by its artifact descriptors"

missing_ownership=$tmp/missing-ownership
cp -R "$fixture" "$missing_ownership"
sed '/^        "objects",$/d' "$missing_ownership/qstar/languages/pack/pack.qsm" > "$missing_ownership/qstar/languages/pack/pack.tmp"
mv "$missing_ownership/qstar/languages/pack/pack.tmp" "$missing_ownership/qstar/languages/pack/pack.qsm"
expect_failure missing-ownership "$qstar" --file "$missing_ownership/qstar.lua" check
contains "$tmp/missing-ownership.err" "needs input ownership 'objects'"

echo "qstar-glp-v2-artifact-contract: ok"
