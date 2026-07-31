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

contains() {
  file=$1
  pattern=$2
  grep -F -q -- "$pattern" "$file" ||
    fail "missing '$pattern' in $file"
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
  "$tmp/project/objects/many" "$tmp/project/objects/quoted" \
  "$tmp/project/qstar/languages" \
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
touch_output() {
  output=$1
  case $output in
    generated/*|build/*)
      mkdir -p "$(dirname "$output")"
      : > "$output"
      ;;
  esac
}
for output in "$@"; do
  case $output in
    @*)
      while IFS= read -r atom; do
        touch_output "$atom"
      done < "${output#@}"
      ;;
    *)
      touch_output "$output"
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
case ${1-} in
  @*)
    rsp=${1#@}
    set -- $(cat "$rsp")
    ;;
esac
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
local no_rsp_args = {}
local huge_no_rsp_args = {}

local REVERSE_OBJECTS = qstar.option "reverse-objects" {
  type = "boolean",
  value = false,
}

local ADD_OBJECT = qstar.option "add-object" {
  type = "boolean",
  value = false,
}

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

if REVERSE_OBJECTS then
  local reversed = {}
  for i = #many_objects, 1, -1 do
    reversed[#reversed + 1] = many_objects[i]
  end
  many_objects = reversed
end

if ADD_OBJECT then
  many_objects[#many_objects + 1] = "objects/many/object-extra.o"
end

for i = 0, 299 do
  no_rsp_args[#no_rsp_args + 1] = string.format("--no-rsp-%03d", i)
end

for i = 0, 399 do
  huge_no_rsp_args[#huge_no_rsp_args + 1] = string.format(
    "--windows-limit-%03d-%s", i, string.rep("x", 96))
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
    archive = qstar.cli {"tools/fake-final.sh", "--driver-mode=wide"},
    link = qstar.cli {"tools/fake-final.sh", "--driver-mode=wide"},
    zig = zig.tools {
      compiler = qstar.cli {"tools/fake-zig.sh"},
    },
  },
  response_files = "on",
  response_style = "posix",
  response_file_tools = {
    "tools/fake-generate.sh",
    "tools/fake-run.sh",
  },
}

qstar.toolset "no_rsp_tools" {
  tools = {
    link = qstar.cli {"tools/fake-final.sh", "--driver-mode=wide"},
  },
  response_files = "off",
  response_style = "posix",
}

qstar.toolset "msvc_rsp_tools" {
  tools = {
    link = qstar.cli {"tools/fake-final.sh", "--driver-mode=wide"},
  },
  response_files = "on",
  response_style = "msvc",
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
  toolset = "//:wide_tools",
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

qstar.executable "no_rsp_small" {
  toolset = "//:no_rsp_tools",
  sources = {direct_objects[1]},
  link_options = no_rsp_args,
}

qstar.executable "no_rsp_limit" {
  toolset = "//:no_rsp_tools",
  sources = {direct_objects[1]},
  link_options = huge_no_rsp_args,
}

qstar.executable "msvc_quoting" {
  toolset = "//:msvc_rsp_tools",
  sources = {"objects/quoted/object with space.o"},
  link_options = {
    "value with space",
    "C:\\\\path\\\\tail\\\\",
    "quote\"inside",
    string.rep("q", 600),
  },
}

qstar.run_target "run_wide" {
  toolset = "//:wide_tools",
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
: > "$tmp/project/objects/many/object-extra.o"
: > "$tmp/project/objects/quoted/object with space.o"

(
  cd "$tmp/project"
  "$qstar" -B build/stella dry-run //:link_1000 \
    > "$tmp/stella-dry.out" 2> "$tmp/stella-dry.err"
  "$qstar" -B build/ninja -G ninja dry-run //:link_1000 \
    > "$tmp/ninja-dry.out" 2> "$tmp/ninja-dry.err"
  "$qstar" -B build/stella dry-run //:wide_generated_command \
    > "$tmp/generated-dry.out" 2> "$tmp/generated-dry.err"
  "$qstar" -B build/stella dry-run //:run_wide \
    > "$tmp/run-dry.out" 2> "$tmp/run-dry.err"
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
  "$qstar" -B build/stella action-log //:link_1000:link:0 \
    > "$tmp/stella-log.out" 2> "$tmp/stella-log.err"
  "$qstar" -B build/ninja -G ninja action-log //:link_1000:link:0 \
    > "$tmp/ninja-log.out" 2> "$tmp/ninja-log.err"
  "$qstar" -B build/stella replay //:link_1000:link:0 \
    > "$tmp/stella-replay.out" 2> "$tmp/stella-replay.err"
  "$qstar" -B build/ninja -G ninja replay //:link_1000:link:0 \
    > "$tmp/ninja-replay.out" 2> "$tmp/ninja-replay.err"
  "$qstar" -B build/stella action-log //:wide_generated_command:generate:0 \
    > "$tmp/generated-log.out" 2> "$tmp/generated-log.err"
  "$qstar" -B build/stella action-log //:run_wide:run:0 \
    > "$tmp/run-log.out" 2> "$tmp/run-log.err"
  "$qstar" -B build/stella build //:no_rsp_small --progress off \
    > "$tmp/no-rsp-small.out" 2> "$tmp/no-rsp-small.err"
  "$qstar" -B build/msvc -G ninja emit-ninja //:msvc_quoting \
    > "$tmp/msvc-emit.out" 2> "$tmp/msvc-emit.err"
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

for backend in stella ninja; do
  dry="$tmp/$backend-dry.out"
  log="$tmp/$backend-log.out"
  contains "$dry" "command_argv id=//:link_1000:link:0 argc=1004"
  contains "$dry" "logical_argc=1004"
  contains "$dry" "object_count=1000"
  contains "$dry" "response=skeleton"
  contains "$dry" "exec_argc=2"
  contains "$log" "logical_argc=1004"
  contains "$log" "exec_argc=2"
  dry_digest=$(field_from_line "$dry" \
    "command_argv id=//:link_1000:link:0" "digest")
  log_digest=$(line_value "$log" "logical_argv_digest")
  test "$dry_digest" = "$log_digest" ||
    fail "$backend dry-run/logical argv digest mismatch"
  dry_rsp_digest=$(field_from_line "$dry" \
    "command_argv id=//:link_1000:link:0" "response_digest")
  log_rsp_digest=$(line_value "$log" "response_digest")
  test "$dry_rsp_digest" = "$log_rsp_digest" ||
    fail "$backend dry-run/response digest mismatch"
  dry_rsp=$(field_from_line "$dry" \
    "command_argv id=//:link_1000:link:0" "response_file")
  log_rsp=$(line_value "$log" "response_file")
  test "$dry_rsp" = "$log_rsp" ||
    fail "$backend dry-run/response path mismatch"
  rsp="$tmp/project/$log_rsp"
  test -f "$rsp" || fail "$backend response file missing"
  test "$(wc -l < "$rsp" | tr -d ' ')" -eq 1003 ||
    fail "$backend response file did not preserve the full logical tail"
  test "$(sed -n '1p' "$rsp")" = "--driver-mode=wide" ||
    fail "$backend response file lost the multi-atom tool role"
  test "$(sed -n '2p' "$rsp")" = "-o" ||
    fail "$backend response file changed output option ordering"
  test "$(sed -n '4p' "$rsp")" = "objects/many/object-0.o" ||
    fail "$backend response file changed first object ordering"
  test "$(tail -n 1 "$rsp")" = "objects/many/object-999.o" ||
    fail "$backend response file changed last object ordering"
done

contains "$tmp/stella-replay.out" "objects/many/object-999.o"
contains "$tmp/ninja-replay.out" "objects/many/object-999.o"
contains "$tmp/generated-dry.out" \
  "command_argv id=//:wide_generated_command:generate:0 argc=302"
contains "$tmp/generated-dry.out" "response=skeleton"
contains "$tmp/generated-log.out" "logical_argc=302"
contains "$tmp/generated-log.out" "exec_argc=2"
contains "$tmp/run-dry.out" "command_argv id=//:run_wide:run:0 argc=301"
contains "$tmp/run-dry.out" "response=skeleton"
contains "$tmp/run-log.out" "logical_argc=301"
contains "$tmp/run-log.out" "exec_argc=2"
contains "$tmp/no-rsp-small.out" \
  "response_file id=//:no_rsp_small:link:0 mode=full capability=off"
test -f "$tmp/project/build/stella/out/___no_rsp_small/no_rsp_small" ||
  fail "response-off action below the host byte limit did not execute"
msvc_rsp="$tmp/project/build/msvc/rsp/___msvc_quoting_link_0.rsp"
test -f "$msvc_rsp" || fail "MSVC-style response file was not materialized"
contains "$msvc_rsp" '"value with space"'
contains "$msvc_rsp" 'C:\path\tail\\'
contains "$msvc_rsp" '"quote\"inside"'
contains "$msvc_rsp" '"objects/quoted/object with space.o"'

printf '%s\n' "content-change" > "$tmp/project/objects/many/object-500.o"
(
  cd "$tmp/project"
  "$qstar" -B build/stella why-rebuild //:link_1000 \
    > "$tmp/content-why.out" 2> "$tmp/content-why.err"
  "$qstar" -B build/stella build //:link_1000 --progress off \
    > "$tmp/content-build.out" 2> "$tmp/content-build.err"
  "$qstar" -D reverse-objects=true -B build/stella why-rebuild //:link_1000 \
    > "$tmp/reverse-why.out" 2> "$tmp/reverse-why.err"
  "$qstar" -D reverse-objects=true -B build/stella dry-run //:link_1000 \
    > "$tmp/reverse-dry.out" 2> "$tmp/reverse-dry.err"
  "$qstar" -D add-object=true -B build/stella dry-run //:link_1000 \
    > "$tmp/add-dry.out" 2> "$tmp/add-dry.err"
)
contains "$tmp/content-why.out" \
  "cache_action id=//:link_1000:link:0 kind=link status=run reason=input-changed"
contains "$tmp/reverse-why.out" \
  "cache_action id=//:link_1000:link:0 kind=link status=run reason=argv-changed"
reverse_digest=$(field_from_line "$tmp/reverse-dry.out" \
  "command_argv id=//:link_1000:link:0" "digest")
add_digest=$(field_from_line "$tmp/add-dry.out" \
  "command_argv id=//:link_1000:link:0" "digest")
base_digest=$(field_from_line "$tmp/stella-dry.out" \
  "command_argv id=//:link_1000:link:0" "digest")
test "$reverse_digest" != "$base_digest" ||
  fail "object ordering did not change the logical argv digest"
test "$add_digest" != "$base_digest" ||
  fail "adding an object did not change the logical argv digest"
contains "$tmp/add-dry.out" "logical_argc=1005"
contains "$tmp/add-dry.out" "object_count=1001"

if (
  cd "$tmp/project"
  "$qstar" --qstar-internal-platform windows -B build/no-rsp-limit \
    build //:no_rsp_limit --progress off \
    > "$tmp/no-rsp-limit.out" 2> "$tmp/no-rsp-limit.err"
); then
  fail "response-off action above the Windows command limit unexpectedly ran"
fi
contains "$tmp/no-rsp-limit.err" \
  "final action '//:no_rsp_limit:link:0'"
contains "$tmp/no-rsp-limit.err" \
  "response files are unavailable under toolset '//:no_rsp_tools'"
contains "$tmp/no-rsp-limit.err" "host command limit is 32767 bytes"
contains "$tmp/no-rsp-limit.err" "argv items and"

if grep -R -E "argv too long|too many object inputs" \
  "$tmp/stella-first.err" "$tmp/stella-cache.err" "$tmp/ninja.err" >/dev/null; then
  fail "fixed argv limit diagnostic reappeared"
fi

printf '%s\n' "qstar-dynamic-action-argv: passed"
