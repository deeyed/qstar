#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
case "$qstar" in
  /*) ;;
  *) qstar=$(pwd)/$qstar ;;
esac
fixture=tests/projects/cxx-build-strategies
tmp=${TMPDIR:-/tmp}/qstar-cxx-build-strategies.$$
last_step=setup

fail() {
  echo "qstar-cxx-build-strategies: $* (step=$last_step)" >&2
  exit 1
}

contains() {
  grep -F -q -- "$2" "$1" || fail "missing '$2' in $1"
}

cleanup() {
  rc=$?
  trap - EXIT HUP INT TERM
  if [ "$rc" -ne 0 ]; then
    find "$tmp" -type f \( -name '*.out' -o -name '*.err' \) -print 2>/dev/null |
      while IFS= read -r file; do
        echo "--- $file" >&2
        tail -n 100 "$file" >&2 || true
      done
  fi
  rm -rf "$tmp"
  exit "$rc"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmp"
stella=$tmp/stella
ninja_project=$tmp/ninja
cp -R "$fixture" "$stella"
cp -R "$fixture" "$ninja_project"
chmod +x "$stella/tools/fake-clang++" "$stella/tools/fake-pch-gen" \
  "$ninja_project/tools/fake-clang++" "$ninja_project/tools/fake-pch-gen"

last_step=graph
"$qstar" --file "$stella/qstar.lua" check //... > "$tmp/check.out" 2> "$tmp/check.err"
"$qstar" --file "$stella/qstar.lua" query //:strategies --format json > "$tmp/query.json" 2> "$tmp/query.err"
contains "$tmp/query.json" '"precompiled_header":"include/pch.hpp"'
contains "$tmp/query.json" '"unity":{"enabled":true,"batch_size":2,"batches":2}'
contains "$tmp/query.json" '"modules":{"enabled":true,"interfaces":["src/math.cppm"]'
contains "$tmp/query.json" '"implementations":["src/alpha.cpp","src/beta.cpp","src/module_use.cpp","src/main.cpp"]'

last_step=dry-run
"$qstar" --file "$stella/qstar.lua" dry-run //:strategies > "$tmp/dry.out" 2> "$tmp/dry.err"
contains "$tmp/dry.out" 'strategy=pch'
contains "$tmp/dry.out" 'id=//:strategies:compile-module-interface-0:0'
contains "$tmp/dry.out" 'id=//:strategies:compile-unity-0:0'
contains "$tmp/dry.out" 'module_output role=bmi'
contains "$tmp/dry.out" '-include-pch'
contains "$tmp/dry.out" '-fprebuilt-module-path='
"$qstar" --file "$stella/qstar.lua" doctor > "$tmp/doctor.out" 2> "$tmp/doctor.err"
contains "$tmp/doctor.out" 'cxx-strategy-capability target=//:strategies'
contains "$tmp/doctor.out" 'family=clang pch=supported unity=supported modules=supported'

last_step=stella
"$qstar" --file "$stella/qstar.lua" --progress off build //:strategies > "$tmp/stella.out" 2> "$tmp/stella.err"
contains "$tmp/stella.out" 'status ok run=5 skip=0 fail=0'
test -f "$stella/build/qstar/out/___strategies/cxx/pch.pch" || fail "missing PCH"
test -f "$stella/build/qstar/out/___strategies/cxx/modules/math.pcm" || fail "missing BMI"
test -f "$stella/build/qstar/out/___strategies/cxx/unity/unity_0.o" || fail "missing unity object"
"$stella/build/qstar/out/___strategies/strategies" || fail "fake Stella executable failed"
contains "$stella/build/qstar/compile_commands.json" '"file":"include/pch.hpp"'
contains "$stella/build/qstar/compile_commands.json" '"file":"src/math.cppm"'
contains "$stella/build/qstar/compile_commands.json" 'unity_0.cpp'
contains "$stella/build/qstar/compile_commands.json" '-fmodule-output='
"$qstar" --file "$stella/qstar.lua" --progress off build //:strategies > "$tmp/stella-repeat.out" 2> "$tmp/stella-repeat.err"
contains "$tmp/stella-repeat.out" 'status ok run=0 skip=5 fail=0'

last_step=generated-pch-stella
PATH="$stella/tools:$PATH" "$qstar" --file "$stella/qstar.lua" --progress off build //:generated_pch > "$tmp/generated-pch-stella.out" 2> "$tmp/generated-pch-stella.err"
test -f "$stella/generated/pch.hpp" || fail "missing generated PCH header"
test -f "$stella/build/qstar/out/___generated_pch/cxx/pch.pch" || fail "missing generated PCH output"

last_step=stella-observability
"$qstar" --file "$stella/qstar.lua" action-log //:strategies:compile-module-interface-0:0 > "$tmp/action-log.out" 2> "$tmp/action-log.err"
contains "$tmp/action-log.out" 'output_count=2'
contains "$tmp/action-log.out" 'math.pcm'
"$qstar" --file "$stella/qstar.lua" replay //:strategies:compile-unity-0:0 > "$tmp/replay.out" 2> "$tmp/replay.err"
contains "$tmp/replay.out" 'unity_0.cpp'

last_step=ninja
command -v ninja >/dev/null 2>&1 || fail "ninja is required"
"$qstar" --file "$ninja_project/qstar.lua" -B build-ninja -G ninja build //:strategies > "$tmp/ninja.out" 2> "$tmp/ninja.err"
contains "$tmp/ninja.out" 'backend ninja'
contains "$tmp/ninja.out" 'status ok'
test -f "$ninja_project/build-ninja/out/___strategies/cxx/pch.pch" || fail "missing Ninja PCH"
test -f "$ninja_project/build-ninja/out/___strategies/cxx/modules/math.pcm" || fail "missing Ninja BMI"
contains "$ninja_project/build-ninja/ninja/build.ninja" '//:strategies:compile-module-interface-0:0'
contains "$ninja_project/build-ninja/ninja/build.ninja" 'unity_0.cpp'
contains "$ninja_project/build-ninja/compile_commands.json" '-include-pch'
"$qstar" --file "$ninja_project/qstar.lua" -B build-ninja -G ninja action-log //:strategies:compile-module-interface-0:0 > "$tmp/ninja-action-log.out" 2> "$tmp/ninja-action-log.err"
contains "$tmp/ninja-action-log.out" 'output_count=2'
contains "$tmp/ninja-action-log.out" 'math.pcm'
PATH="$ninja_project/tools:$PATH" "$qstar" --file "$ninja_project/qstar.lua" -B build-ninja -G ninja build //:generated_pch > "$tmp/generated-pch-ninja.out" 2> "$tmp/generated-pch-ninja.err"
test -f "$ninja_project/generated/pch.hpp" || fail "missing Ninja generated PCH header"
test -f "$ninja_project/build-ninja/out/___generated_pch/cxx/pch.pch" || fail "missing Ninja generated PCH output"

last_step=capability-diagnostics
cp -R "$fixture" "$tmp/unsupported"
chmod +x "$tmp/unsupported/tools/fake-clang++" "$tmp/unsupported/tools/fake-pch-gen"
sed 's/fake-clang++/opaque-cxx/g' "$tmp/unsupported/qstar.lua" > "$tmp/unsupported/qstar.lua.new"
mv "$tmp/unsupported/qstar.lua.new" "$tmp/unsupported/qstar.lua"
cp "$tmp/unsupported/tools/fake-clang++" "$tmp/unsupported/tools/opaque-cxx"
chmod +x "$tmp/unsupported/tools/opaque-cxx"
if "$qstar" --file "$tmp/unsupported/qstar.lua" build //:strategies > "$tmp/unsupported.out" 2> "$tmp/unsupported.err"; then
  fail "unknown C++ compiler capability unexpectedly succeeded"
fi
contains "$tmp/unsupported.err" 'unknown capability family'

cp -R "$fixture" "$tmp/gcc-modules"
mv "$tmp/gcc-modules/tools/fake-clang++" "$tmp/gcc-modules/tools/fake-g++"
chmod +x "$tmp/gcc-modules/tools/fake-g++"
sed 's/fake-clang++/fake-g++/g' "$tmp/gcc-modules/qstar.lua" > "$tmp/gcc-modules/qstar.lua.new"
mv "$tmp/gcc-modules/qstar.lua.new" "$tmp/gcc-modules/qstar.lua"
if "$qstar" --file "$tmp/gcc-modules/qstar.lua" build //:strategies > "$tmp/gcc-modules.out" 2> "$tmp/gcc-modules.err"; then
  fail "GCC C++ modules unexpectedly succeeded"
fi
contains "$tmp/gcc-modules.err" 'lang.cxx.modules requires Clang in this release'
contains "$tmp/gcc-modules.err" 'classified as gcc'

mkdir -p "$tmp/bad-schema"
cat > "$tmp/bad-schema/qstar.lua" <<'EOF'
qstar.executable "bad" {
  sources = {"main.cpp"},
  lang = {
    cxx = {
      unity = { enabled = true, jumbo = true },
    },
  },
}
EOF
if "$qstar" --file "$tmp/bad-schema/qstar.lua" check > "$tmp/bad-schema.out" 2> "$tmp/bad-schema.err"; then
  fail "unknown C++ unity strategy field unexpectedly succeeded"
fi
contains "$tmp/bad-schema.err" "unknown field 'jumbo'"

last_step=legacy-cxx
cp -R tests/projects/cxx-mixed "$tmp/cxx-mixed"
if command -v c++ >/dev/null 2>&1; then
  "$qstar" --file "$tmp/cxx-mixed/qstar.lua" build //:mixed > "$tmp/cxx-mixed.out" 2> "$tmp/cxx-mixed.err"
  contains "$tmp/cxx-mixed.out" 'status ok'
fi

last_step=real-clang
real_clang=${QSTAR_REAL_CXX_CLANG:-}
if [ -z "$real_clang" ] && [ -x /opt/homebrew/opt/llvm@20/bin/clang++ ]; then
  real_clang=/opt/homebrew/opt/llvm@20/bin/clang++
fi
if [ -z "$real_clang" ] && command -v clang++ >/dev/null 2>&1; then
  real_clang=$(command -v clang++)
fi
if [ -n "$real_clang" ]; then
  mkdir -p "$tmp/clang-probe"
  printf '%s\n' 'export module qstar_probe;' > "$tmp/clang-probe/probe.cppm"
  if "$real_clang" -std=c++20 -c "$tmp/clang-probe/probe.cppm" \
      -o "$tmp/clang-probe/probe.o" \
      -fmodule-output="$tmp/clang-probe/probe.pcm" >/dev/null 2>&1; then
    cp -R "$fixture" "$tmp/real-clang"
    chmod +x "$tmp/real-clang/tools/fake-clang++" "$tmp/real-clang/tools/fake-pch-gen"
    sed "s#tools/fake-clang++#$real_clang#g" "$tmp/real-clang/qstar.lua" > "$tmp/real-clang/qstar.lua.new"
    mv "$tmp/real-clang/qstar.lua.new" "$tmp/real-clang/qstar.lua"
    "$qstar" --file "$tmp/real-clang/qstar.lua" --progress off build //:strategies > "$tmp/real-clang.out" 2> "$tmp/real-clang.err"
    "$tmp/real-clang/build/qstar/out/___strategies/strategies" || fail "real Clang strategies executable failed"
  fi
fi

last_step=real-gcc
real_gxx=${QSTAR_REAL_CXX_GCC:-}
if [ -z "$real_gxx" ] && command -v g++ >/dev/null 2>&1 &&
    ! g++ --version 2>/dev/null | grep -qi clang; then
  real_gxx=$(command -v g++)
fi
if [ -n "$real_gxx" ]; then
  cp -R "$fixture" "$tmp/real-gcc"
  cat > "$tmp/real-gcc/qstar.lua" <<EOF
qstar.project {
  name = "cxx-gcc-strategies",
  root = ".",
  compile_commands = "build",
}
qstar.toolset "gcc" {
  allow_absolute_tools = "on",
  tools = {
    cxx = { compiler = qstar.cli {"$real_gxx"} },
    link = qstar.cli {"$real_gxx"},
  },
}
qstar.executable "strategies" {
  toolset = "//:gcc",
  sources = {"src/alpha.cpp", "src/beta.cpp", "src/main_gcc.cpp"},
  lang = {
    cxx = {
      standard = "c++17",
      precompiled_header = "include/pch.hpp",
      unity = { enabled = true, batch_size = 2 },
      include_dirs = {"include"},
    },
  },
}
EOF
  "$qstar" --file "$tmp/real-gcc/qstar.lua" --progress off build //:strategies > "$tmp/real-gcc.out" 2> "$tmp/real-gcc.err"
  "$tmp/real-gcc/build/qstar/out/___strategies/strategies" || fail "real GCC strategies executable failed"
fi

last_step=done
echo "qstar-cxx-build-strategies: ok"
