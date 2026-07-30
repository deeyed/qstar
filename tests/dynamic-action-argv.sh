#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
qstar=${QSTAR_TEST_QSTAR:-"$root/build/bin/qstar"}
cc=${CC:-cc}
tmp=${TMPDIR:-/tmp}/qstar-dynamic-action-argv-$$
direct_count=${QSTAR_DYNAMIC_ARGV_DIRECT_COUNT:-300}
object_count=${QSTAR_DYNAMIC_ARGV_OBJECT_COUNT:-1000}
generated_count=${QSTAR_DYNAMIC_ARGV_GENERATED_COUNT:-300}
action_arg_count=${QSTAR_DYNAMIC_ARGV_ACTION_COUNT:-300}

fail() {
  printf '%s\n' "qstar-dynamic-action-argv: $*" >&2
  exit 1
}

cleanup() {
  status=$?
  if [ "$status" -ne 0 ]; then
    for log in "$tmp"/*.err; do
      test -f "$log" && cat "$log" >&2
    done
  fi
  if [ "${QSTAR_KEEP_TEST_TMP:-0}" = "1" ]; then
    printf '%s\n' "qstar-dynamic-action-argv: kept $tmp" >&2
    return
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

for count in "$direct_count" "$object_count" "$generated_count" \
  "$action_arg_count"; do
  case $count in
    ''|*[!0-9]*|0)
      fail "fixture counts must be positive integers"
      ;;
  esac
done
direct_max=$((direct_count - 1))
object_max=$((object_count - 1))
generated_max=$((generated_count - 1))
action_arg_max=$((action_arg_count - 1))

mkdir -p "$tmp/unit" "$tmp/project/tools" "$tmp/project/objects/direct" \
  "$tmp/project/objects/many" "$tmp/project/qstar/languages" \
  "$tmp/project/src"
cp -R "$root/qstar/languages/zig" "$tmp/project/qstar/languages/zig"

"$cc" -std=c99 -Wall -Wextra -Wpedantic -Werror \
  -I"$root/include" -I"$root/src" \
  "$root/tests/argv-unit.c" "$root/src/argv.c" \
  -o "$tmp/unit/argv-unit"
"$tmp/unit/argv-unit" > "$tmp/unit.out"
grep -F "qstar-argv-unit: passed" "$tmp/unit.out" >/dev/null ||
  fail "internal argv unit test did not pass"

cat > "$tmp/project/tools/fake-generate.sh" <<'EOF'
#!/bin/sh
set -eu
for output in "$@"; do
  case $output in
    generated/*|build/*)
      mkdir -p "$(dirname "$output")"
      : > "$output"
      ;;
  esac
done
EOF

cat > "$tmp/project/tools/fake-final.sh" <<'EOF'
#!/bin/sh
set -eu
case ${1-} in
  @*)
    rsp=${1#@}
    set -- $(cat "$rsp")
    ;;
esac
output=
previous=
archive_next=false
for atom in "$@"; do
  if [ "$archive_next" = true ]; then
    output=$atom
    archive_next=false
    break
  fi
  if [ "$atom" = "rcs" ]; then
    archive_next=true
  elif [ "$previous" = "-o" ]; then
    output=$atom
    break
  fi
  previous=$atom
done
test -n "$output"
mkdir -p "$(dirname "$output")"
printf '%s\n' '#!/bin/sh' 'exit 0' > "$output"
chmod +x "$output"
EOF

cat > "$tmp/project/tools/fake-run.sh" <<'EOF'
#!/bin/sh
set -eu
mkdir -p generated
printf '%s\n' "$#" > generated/wide-run-count
EOF

cat > "$tmp/project/tools/fake-zig.sh" <<'EOF'
#!/bin/sh
set -eu
output=
parse_atom() {
  case $1 in
    -femit-bin=*)
      output=${1#-femit-bin=}
      ;;
  esac
}
while [ "$#" -gt 0 ]; do
  case $1 in
    @*)
      while IFS= read -r atom; do
        parse_atom "$atom"
      done < "${1#@}"
      ;;
    *)
      parse_atom "$1"
      ;;
  esac
  shift
done
test -n "$output"
mkdir -p "$(dirname "$output")"
printf '%s\n' "fake zig artifact" > "$output"
EOF
chmod +x "$tmp/project/tools/fake-generate.sh" \
  "$tmp/project/tools/fake-final.sh" "$tmp/project/tools/fake-run.sh" \
  "$tmp/project/tools/fake-zig.sh"
printf '%s\n' "pub fn main() void {}" > "$tmp/project/src/main.zig"

i=0
while [ "$i" -lt "$direct_count" ]; do
  : > "$tmp/project/objects/direct/object-$i.o"
  i=$((i + 1))
done
i=0
while [ "$i" -lt "$object_count" ]; do
  : > "$tmp/project/objects/many/object-$i.o"
  i=$((i + 1))
done

cat > "$tmp/project/qstar.lua" <<EOF
local direct_objects = {}
local many_objects = {}
local generated_outputs = {}
local wide_generated_args = {}
local wide_provider_args = {}
local wide_run_command = {"tools/fake-run.sh"}

local zig = qstar.use_language("zig")

for i = 0, $direct_max do
  direct_objects[#direct_objects + 1] =
    string.format("objects/direct/object-%d.o", i)
end

for i = 0, $generated_max do
  local output = qstar.output(string.format(
    "generated/wide/object-%d.o", i),
    {format = "object"})
  generated_outputs[#generated_outputs + 1] = output
  qstar.custom_target("generate_object_" .. i) {
    outputs = {output},
    command = qstar.cli {
      "tools/fake-generate.sh",
      qstar.output(0),
    },
  }
end

for i = 0, $action_arg_max do
  wide_generated_args[#wide_generated_args + 1] =
    string.format("argv-atom-%d", i)
  wide_provider_args[#wide_provider_args + 1] =
    string.format("--provider-atom-%d", i)
  wide_run_command[#wide_run_command + 1] =
    string.format("run-atom-%d", i)
end

for i = 0, $object_max do
  many_objects[#many_objects + 1] =
    string.format("objects/many/object-%d.o", i)
end

local wide_generate_command = {"tools/fake-generate.sh"}
for _, atom in ipairs(wide_generated_args) do
  wide_generate_command[#wide_generate_command + 1] = atom
end
wide_generate_command[#wide_generate_command + 1] = qstar.output(0)

qstar.project {
  name = "dynamic-action-argv",
  root = ".",
}

qstar.toolset "wide_tools" {
  tools = {
    archive = qstar.cli {"tools/fake-final.sh"},
    link = qstar.cli {"tools/fake-final.sh"},
    zig = zig.tools {
      compiler = qstar.cli {"tools/fake-zig.sh"},
    },
  },
  response_files = "on",
  response_style = "posix",
}

qstar.config "wide_provider" {
  toolset = "//:wide_tools",
  lang = {
    zig = zig.options {
      compile_options = wide_provider_args,
    },
  },
}

qstar.custom_target "wide_generated_command" {
  outputs = {qstar.output("generated/wide-command.stamp")},
  command = qstar.cli(wide_generate_command),
}

qstar.objectlib "many_objects" {
  sources = many_objects,
}

qstar.objectlib "generated_objects" {
  sources = generated_outputs,
}

qstar.staticlib "archive_1000" {
  toolset = "//:wide_tools",
  objects = {"//:many_objects"},
}

qstar.executable "link_1000" {
  toolset = "//:wide_tools",
  objects = {"//:many_objects"},
}

qstar.executable "direct_300" {
  toolset = "//:wide_tools",
  sources = direct_objects,
}

qstar.executable "generated_300" {
  toolset = "//:wide_tools",
  objects = {"//:generated_objects"},
}

qstar.executable "provider_wide" {
  configs = {"//:wide_provider"},
  sources = {"src/main.zig"},
}

qstar.run_target "run_wide" {
  command = qstar.cli(wide_run_command),
}

qstar.test "test_hook_wide" {
  toolset = "//:wide_tools",
  sources = {direct_objects[1]},
  setup = qstar.cli(wide_run_command),
}

qstar.group "all" {
  deps = {
    "//:wide_generated_command",
    "//:archive_1000",
    "//:link_1000",
    "//:direct_300",
    "//:generated_300",
    "//:provider_wide",
    "//:run_wide",
  },
}
EOF

(
  cd "$tmp/project"
  "$qstar" -B build/stella build //:all --progress off --schedule-trace \
    > "$tmp/stella-first.out" 2> "$tmp/stella-first.err"
  "$qstar" -B build/stella build //:all --progress off --schedule-trace \
    > "$tmp/stella-cache.out" 2> "$tmp/stella-cache.err"
  "$qstar" -B build/ninja -G ninja build //:all --progress off \
    > "$tmp/ninja.out" 2> "$tmp/ninja.err"
  "$qstar" -B build/test-stella -G stella test //:test_hook_wide \
    > "$tmp/test-stella.out" 2> "$tmp/test-stella.err"
  "$qstar" -B build/test-ninja -G ninja test //:test_hook_wide \
    > "$tmp/test-ninja.out" 2> "$tmp/test-ninja.err"
)

grep -F "lowered_action id=//:link_1000:link:0 status=hit" \
  "$tmp/stella-cache.out" >/dev/null ||
  fail "cached final action with 1000 objects was not restored"
grep -F "status ok" "$tmp/stella-first.out" >/dev/null ||
  fail "Stella wide argv build failed"
grep -F "status ok" "$tmp/ninja.out" >/dev/null ||
  fail "Ninja wide argv build failed"
grep -F "test_result label=//:test_hook_wide status=pass" \
  "$tmp/test-stella.out" >/dev/null ||
  fail "Stella wide test hook failed"
grep -F "test_result label=//:test_hook_wide status=pass" \
  "$tmp/test-ninja.out" >/dev/null ||
  fail "Ninja wide test hook failed"
test -f "$tmp/project/build/stella/out/___link_1000/link_1000" ||
  fail "Stella 1000-object link artifact missing"
test -f "$tmp/project/build/stella/out/___archive_1000/libarchive_1000.a" ||
  fail "Stella 1000-object archive artifact missing"
test -f "$tmp/project/build/ninja/out/___link_1000/link_1000" ||
  fail "Ninja 1000-object link artifact missing"
test -f "$tmp/project/build/ninja/out/___archive_1000/libarchive_1000.a" ||
  fail "Ninja 1000-object archive artifact missing"
test -f "$tmp/project/generated/wide-command.stamp" ||
  fail "wide generated action output missing"
test -f "$tmp/project/generated/wide-run-count" ||
  fail "wide run action marker missing"
test "$(cat "$tmp/project/generated/wide-run-count")" -eq "$action_arg_count" ||
  fail "wide run action did not receive $action_arg_count arguments"

if grep -R -E "argv too long|too many object inputs" \
  "$tmp/stella-first.err" "$tmp/stella-cache.err" "$tmp/ninja.err" >/dev/null; then
  fail "fixed argv limit diagnostic reappeared"
fi

printf '%s\n' "qstar-dynamic-action-argv: passed"
