#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
case "$qstar" in
/*) ;;
*) qstar="$(pwd)/$qstar" ;;
esac

tmp=${TMPDIR:-/tmp}/qstar-malformed-declarations.$$
project=$tmp/project
finish() {
	rm -rf "$tmp"
}
trap finish EXIT HUP INT TERM
mkdir -p "$tmp"
cp -R tests/corpus/malformed-declarations "$project"

contains() {
	file=$1
	pattern=$2
	if ! grep -F -- "$pattern" "$file" >/dev/null; then
		echo "qstar-malformed-declarations: missing pattern '$pattern' in $file" >&2
		cat "$file" >&2
		exit 1
	fi
}

run_case() {
	name=$1
	api=$2
	label=$3
	detail=$4
	line=$(awk -v marker="CASE:$name" 'index($0, marker) { print NR; exit }' "$project/qstar.lua")
	if "$qstar" --file "$project/qstar.lua" -D "malformed_case=$name" check \
	    > "$tmp/$name.out" 2> "$tmp/$name.err"; then
		echo "qstar-malformed-declarations: $name unexpectedly succeeded" >&2
		exit 1
	fi
	contains "$tmp/$name.err" "$project/qstar.lua:$line:"
	contains "$tmp/$name.err" "$api declaration '$label'"
	contains "$tmp/$name.err" "$detail"
}

"$qstar" --file "$project/qstar.lua" check > "$tmp/ok.out" 2> "$tmp/ok.err"
contains "$tmp/ok.out" "status ok"

run_case project_unknown qstar.project malformed "unknown field 'typo'"
run_case toolset_type qstar.toolset //:bad_tools "field 'path_tools' must be list, got string"
run_case toolset_unknown qstar.toolset //:bad_tools "unknown field 'path_tool'"
run_case config_unknown qstar.config //:bad_config "unknown field 'typo'"
run_case artifact_type qstar.executable //:bad_app "field 'sources' must be list, got string"
run_case objectlib_forbidden qstar.objectlib //:bad_objects "unknown field 'link_options'"
run_case group_unknown qstar.group //:bad_group "unknown field 'sources'"
run_case stage_type qstar.stage //:bad_stage "field 'files' must be list, got string"
run_case stage_unknown qstar.stage //:bad_stage "unknown field 'file'"
run_case target_family_type qstar.target_family bad_family "field 'allow_shared_sources' must be boolean, got string"
run_case target_family_unknown qstar.target_family bad_family "unknown field 'target'"
run_case custom_target_unknown qstar.custom_target //:bad_generated "unknown field 'typo'"
run_case transform_type qstar.transform //:bad_transform "field 'output' must be string or qstar.output(...), got boolean"
run_case configure_unknown qstar.configure_file //:bad_configure "unknown field 'typo'"
run_case run_target_type qstar.run_target //:bad_run "field 'timeout' must be integer, got string"
run_case command_type qstar.command bad_command "field 'hidden' must be boolean, got string"
run_case command_unknown qstar.command bad_command "unknown field 'step'"
run_case command_step_mutated "qstar.command step" "bad_command[1].build" "unknown field 'typo'"
run_case command_option_unknown qstar.param string "unknown field 'typo'"
run_case option_type qstar.option bad_option "field 'type' must be string, got boolean"
run_case option_unknown qstar.option bad_option "unknown field 'default'"
run_case variant_unknown qstar.variant bad_variant "unknown field 'typo'"
run_case list_shape qstar.executable //:bad_list "field 'sources' must be a list with contiguous integer indexes starting at 1"

printf 'qstar-malformed-declarations: passed\n'
