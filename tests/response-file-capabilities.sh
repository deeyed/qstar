#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
case "$qstar" in
/*) ;;
*) qstar="$(pwd)/$qstar" ;;
esac

tmp=${TMPDIR:-/tmp}/qstar-response-capability.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/tools" "$tmp/src"

cat > "$tmp/src/main.c" <<'C'
int main(void)
{
	return 0;
}
C

cat > "$tmp/tools/direct.py" <<'PY'
import pathlib
import sys

if any(arg.startswith("@") for arg in sys.argv[1:]):
    raise SystemExit("unexpected response file")
output = pathlib.Path(sys.argv[-1])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(f"direct argc={len(sys.argv)}\n", encoding="utf-8")
PY

cat > "$tmp/tools/direct_run.py" <<'PY'
import sys

if any(arg.startswith("@") for arg in sys.argv[1:]):
    raise SystemExit("unexpected response file")
print(f"DIRECT_RUN_OK argc={len(sys.argv)}")
PY

cat > "$tmp/tools/rsp_tool.py" <<'PY'
#!/usr/bin/env python3
import pathlib
import shlex
import sys

if len(sys.argv) != 2 or not sys.argv[1].startswith("@"):
    raise SystemExit("expected one response-file argument")
args = []
for line in pathlib.Path(sys.argv[1][1:]).read_text(encoding="utf-8").splitlines():
    args.extend(shlex.split(line))
try:
    output = pathlib.Path(args[args.index("--output") + 1])
except (ValueError, IndexError):
    raise SystemExit("response file has no output")
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(f"response argc={len(args)}\n", encoding="utf-8")
PY
chmod +x "$tmp/tools/rsp_tool.py"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.project {
  name = "response-capability",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/generated",
  compile_commands = "off",
}

qstar.toolset "host" {
  tools = {
    c = {compiler = qstar.cli {"cc"}},
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  response_files = "on",
  response_style = "posix",
  path_tools = {"python3"},
  response_file_tools = {"tools/rsp_tool.py"},
}

local function long_command(tool, output)
  local argv = {tool}
  if tool == "python3" then
    argv[#argv + 1] = "tools/direct.py"
  end
  for i = 1, 64 do
    argv[#argv + 1] = string.format("argument-%03d-abcdefghijklmnopqrstuvwxyz", i)
  end
  if tool == "tools/rsp_tool.py" then
    argv[#argv + 1] = "--output"
  end
  argv[#argv + 1] = output
  return qstar.cli(argv)
end

qstar.custom_target "direct_generated" {
  toolset = "//:host",
  outputs = {qstar.output("build/generated/direct.txt")},
  command = long_command("python3", qstar.output(0)),
}

qstar.custom_target "rsp_generated" {
  toolset = "//:host",
  outputs = {qstar.output("build/generated/response.txt")},
  command = long_command("tools/rsp_tool.py", qstar.output(0)),
}

local function long_run_command()
  local argv = {"python3", "tools/direct_run.py"}
  for i = 1, 64 do
    argv[#argv + 1] = string.format("run-argument-%03d-abcdefghijklmnopqrstuvwxyz", i)
  end
  return qstar.cli(argv)
end

qstar.run_target "leaf" {
  toolset = "//:host",
  command = long_run_command(),
  expect = {contains = "DIRECT_RUN_OK"},
}

qstar.group "aggregate" {
  deps = {"//:leaf"},
}

qstar.run_target "consumer" {
  toolset = "//:host",
  deps = {"//:aggregate"},
  command = long_run_command(),
  expect = {contains = "DIRECT_RUN_OK"},
}

qstar.run_target "direct_consumer" {
  toolset = "//:host",
  deps = {"//:leaf"},
  command = long_run_command(),
  expect = {contains = "DIRECT_RUN_OK"},
}

qstar.executable "after_run" {
  toolset = "//:host",
  sources = {"src/main.c"},
  deps = {"//:leaf"},
}
EOF

cd "$tmp"

"$qstar" --file qstar.lua dry-run //:direct_generated > direct.dry
grep -F "id=//:direct_generated:generate:0" direct.dry >/dev/null
grep -F "response=none" direct.dry >/dev/null
grep -F "response_capability=off" direct.dry >/dev/null

"$qstar" --file qstar.lua dry-run //:rsp_generated > response.dry
grep -F "id=//:rsp_generated:generate:0" response.dry >/dev/null
grep -F "response=skeleton" response.dry >/dev/null

"$qstar" --file qstar.lua dry-run //:consumer > consumer.dry
grep -F "id=//:leaf:run:0" consumer.dry >/dev/null
grep -F "id=//:consumer:run:0" consumer.dry >/dev/null
grep -F "response_capability=off" consumer.dry >/dev/null
if grep -F "has no artifact" consumer.dry >/dev/null; then
  echo "completion dependency was lowered as an artifact" >&2
  exit 1
fi

"$qstar" --file qstar.lua -B build/stella build //:direct_generated --progress off
"$qstar" --file qstar.lua -B build/stella build //:rsp_generated --progress off
"$qstar" --file qstar.lua -B build/stella build //:consumer --progress off
"$qstar" --file qstar.lua -B build/stella build //:direct_consumer --progress off
"$qstar" --file qstar.lua -B build/stella build //:after_run --progress off
test -f build/generated/direct.txt
test -f build/generated/response.txt

rm -f build/generated/direct.txt build/generated/response.txt
"$qstar" --file qstar.lua -B build/ninja -G ninja build //:direct_generated --progress off
if grep -R "python3 @.*rsp" build/ninja/ninja >/dev/null 2>&1; then
  echo "Ninja materialized an arbitrary Python action through @rsp" >&2
  exit 1
fi
"$qstar" --file qstar.lua -B build/ninja -G ninja build //:rsp_generated --progress off
grep -R "rsp_tool.py @.*rsp" build/ninja/ninja >/dev/null
"$qstar" --file qstar.lua -B build/ninja -G ninja build //:consumer --progress off
"$qstar" --file qstar.lua -B build/ninja -G ninja build //:direct_consumer --progress off
"$qstar" --file qstar.lua -B build/ninja -G ninja build //:after_run --progress off
test -f build/generated/direct.txt
test -f build/generated/response.txt

printf '%s\n' "response-file-capabilities: ok"
