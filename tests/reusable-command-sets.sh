#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
case "$qstar" in
/*) ;;
*) qstar="$(pwd)/$qstar" ;;
esac

tmp=${TMPDIR:-/tmp}/qstar-reusable-command-sets.$$
project=$tmp/project
finish() {
	rm -rf "$tmp"
}
trap finish EXIT HUP INT TERM
mkdir -p "$tmp"
cp -R tests/projects/reusable-command-sets "$project"
chmod +x "$project/tools/write-command.sh"

fail() {
	printf 'qstar-reusable-command-sets: %s\n' "$1" >&2
	exit 1
}

contains() {
	file=$1
	pattern=$2
	grep -F -q -- "$pattern" "$file" ||
		fail "missing pattern '$pattern' in $file"
}

not_contains() {
	file=$1
	pattern=$2
	if grep -F -q -- "$pattern" "$file"; then
		fail "unexpected pattern '$pattern' in $file"
	fi
}

run_capture() {
	name=$1
	shift
	if ! "$@" > "$tmp/$name.out" 2> "$tmp/$name.err"; then
		printf '%s\n' "--- $name.out ---" >&2
		cat "$tmp/$name.out" >&2 || true
		printf '%s\n' "--- $name.err ---" >&2
		cat "$tmp/$name.err" >&2 || true
		fail "$name failed"
	fi
}

run_failure() {
	name=$1
	shift
	if "$@" > "$tmp/$name.out" 2> "$tmp/$name.err"; then
		fail "$name unexpectedly succeeded"
	fi
}

cd "$project"
run_capture commands "$qstar" commands
contains "$tmp/commands.out" "qstar commands v1"
contains "$tmp/commands.out" "command verify-set default=false aliases=[verify-spec] options=3 steps=1"
contains "$tmp/commands.out" "command pipeline default=true aliases=[] options=0 steps=1"
contains "$tmp/commands.out" "command batch-60 default=false aliases=[b60] options=0 steps=1"
contains "$tmp/commands.out" "command direct default=false aliases=[] options=0 steps=1"

run_capture commands_json "$qstar" commands --format json
contains "$tmp/commands_json.out" '"schema":"qstar-commands-v1"'
contains "$tmp/commands_json.out" '"command_count":63'
contains "$tmp/commands_json.out" '"name":"verify-set"'
contains "$tmp/commands_json.out" '"aliases":["verify-spec"]'
contains "$tmp/commands_json.out" '"name":"batch-60"'
contains "$tmp/commands_json.out" '"name":"direct"'

run_capture default "$qstar" --progress off
contains "$tmp/default.out" "command pipeline"
contains "$tmp/default.out" "command batch-01"
contains "$tmp/default.out" "command_status pipeline ok"

run_capture stella "$qstar" -G stella --progress off verify-spec \
	--mode release --verbose --out generated/stella.txt
contains "$tmp/stella.out" "command verify-set"
contains "$tmp/stella.out" "command_status verify-set ok"
contains generated/stella.txt "input=reusable fixture"
contains generated/stella.txt "mode=release"
contains generated/stella.txt "verbose=--verbose"
contains generated/stella.txt "command_env=module"
contains generated/stella.txt "step_env=run"

run_capture ninja "$qstar" -G ninja --progress off verify-set \
	--mode debug --out generated/ninja.txt
contains "$tmp/ninja.out" "command_status verify-set ok"
contains generated/ninja.txt "mode=debug"
contains generated/ninja.txt "command_env=module"

run_capture pipeline "$qstar" --progress off pipeline
contains "$tmp/pipeline.out" "command pipeline"
contains "$tmp/pipeline.out" "command batch-01"
contains "$tmp/pipeline.out" "command_status pipeline ok"
run_capture batch "$qstar" --progress off b60
contains "$tmp/batch.out" "command batch-60"
run_capture direct "$qstar" --progress off direct
contains "$tmp/direct.out" "command direct"

cd "$tmp"
mkdir -p immutable/qstar/modules/commands
cp -R "$project/qstar/modules/commands/commands.qsm" \
	immutable/qstar/modules/commands/commands.qsm
cat > immutable/qstar.lua <<'EOF'
local specs = qstar.import_module("qstar/modules/commands")
specs[1].options.mode.default = "release"
qstar.command_set(specs)
EOF
run_failure immutable "$qstar" --file immutable/qstar.lua commands
contains "$tmp/immutable.err" "qstar.command_spec is read-only: default"

mkdir -p fragment/leaf
cat > fragment/qstar.lua <<'EOF'
qstar.subdir("leaf")
EOF
cat > fragment/leaf/leaf.qst <<'EOF'
qstar.command_set {
  qstar.command_spec "leaf" {
    steps = {qstar.step.check("//...")},
  },
}
EOF
run_failure fragment "$qstar" --file fragment/qstar.lua commands
contains "$tmp/fragment.err" "qstar.command_set is only allowed in root qstar.lua"

mkdir -p module-materialize/qstar/modules/bad
cat > module-materialize/qstar.lua <<'EOF'
qstar.import_module("qstar/modules/bad")
EOF
cat > module-materialize/qstar/modules/bad/bad.qsm <<'EOF'
local specs = {
  qstar.command_spec "bad" {
    steps = {qstar.step.check("//...")},
  },
}
qstar.command_set(specs)
return specs
EOF
run_failure module_materialize "$qstar" \
	--file module-materialize/qstar.lua commands
contains "$tmp/module_materialize.err" \
	"qstar.command_set is forbidden inside .qsm module"

mkdir -p plain
cat > plain/qstar.lua <<'EOF'
qstar.command_set {
  {
    name = "plain",
    steps = {qstar.step.check("//...")},
  },
}
EOF
run_failure plain "$qstar" --file plain/qstar.lua commands
contains "$tmp/plain.err" \
	"qstar.command_set item 1 must be an immutable qstar.command_spec value"

mkdir -p duplicate
cat > duplicate/qstar.lua <<'EOF'
qstar.command "same" {
  steps = {qstar.step.check("//...")},
}
qstar.command_set {
  qstar.command_spec "same" {
    steps = {qstar.step.check("//...")},
  },
}
EOF
run_failure duplicate "$qstar" --file duplicate/qstar.lua commands
contains "$tmp/duplicate.err" "duplicate project command 'same'"

mkdir -p alias-collision
cat > alias-collision/qstar.lua <<'EOF'
qstar.command_set {
  qstar.command_spec "one" {
    aliases = {"shared"},
    steps = {qstar.step.check("//...")},
  },
  qstar.command_spec "two" {
    aliases = {"shared"},
    steps = {qstar.step.check("//...")},
  },
}
EOF
run_failure alias_collision "$qstar" --file alias-collision/qstar.lua commands
contains "$tmp/alias_collision.err" "duplicate project command alias 'shared'"

mkdir -p cycle
cat > cycle/qstar.lua <<'EOF'
qstar.command_set {
  qstar.command_spec "cycle-a" {
    steps = {qstar.step.call("cycle-b")},
  },
  qstar.command_spec "cycle-b" {
    steps = {qstar.step.call("cycle-a")},
  },
}
EOF
run_failure cycle "$qstar" --file cycle/qstar.lua commands
contains "$tmp/cycle.err" "project command call cycle includes"

mkdir -p malformed-list
cat > malformed-list/qstar.lua <<'EOF'
local specs = {
  qstar.command_spec "valid" {
    steps = {qstar.step.check("//...")},
  },
}
specs.extra = qstar.command_spec "extra" {
  steps = {qstar.step.check("//...")},
}
qstar.command_set(specs)
EOF
run_failure malformed_list "$qstar" --file malformed-list/qstar.lua commands
contains "$tmp/malformed_list.err" \
	"qstar.command_set expects a contiguous list starting at index 1"

mkdir -p malformed-spec
cat > malformed-spec/qstar.lua <<'EOF'
qstar.command_set {
  qstar.command_spec "bad" {
    step = qstar.step.check("//..."),
  },
}
EOF
run_failure malformed_spec "$qstar" --file malformed-spec/qstar.lua commands
contains "$tmp/malformed_spec.err" \
	"qstar.command_spec declaration 'bad': unknown field 'step'"

mkdir -p malformed-step
cat > malformed-step/qstar.lua <<'EOF'
local step = qstar.step.check("//...")
step.typo = true
qstar.command_set {
  qstar.command_spec "bad-step" {
    steps = {step},
  },
}
EOF
run_failure malformed_step "$qstar" --file malformed-step/qstar.lua commands
contains "$tmp/malformed_step.err" \
	"qstar.command step declaration 'bad-step[1].check': unknown field 'typo'"

printf 'qstar-reusable-command-sets: passed\n'
