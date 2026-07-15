#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
case "$qstar" in
/*) ;;
*) qstar="$(pwd)/$qstar" ;;
esac

tmp=${TMPDIR:-/tmp}/qstar-composable-test-suites.$$
project=$tmp/project
finish() {
	rm -rf "$tmp"
}
trap finish EXIT HUP INT TERM
mkdir -p "$tmp"
cp -R tests/projects/composable-test-suites "$project"
chmod +x "$project/tools/probe.sh"

fail() {
	printf 'qstar-composable-test-suites: %s\n' "$1" >&2
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
run_capture check "$qstar" check //...
contains "$tmp/check.out" "status ok"

run_capture targets "$qstar" list-targets --format json
contains "$tmp/targets.out" '"test_suite_count":4'
contains "$tmp/targets.out" '"label":"//:verification"'
contains "$tmp/targets.out" '"tests":["//:host_units","//:emulator_runs"]'
contains "$tmp/targets.out" '"resolved_tests":["//:scheduler_unit","//:queue_unit","//:emulator_smoke"]'
contains "$tmp/targets.out" '"label":"//:scheduler_unit"'
contains "$tmp/targets.out" '"direct_test_suites":["//:host_units"]'
contains "$tmp/targets.out" '"test_suites":["//:host_units","//:verification"]'

run_capture query_suite "$qstar" query //:verification
contains "$tmp/query_suite.out" "test_suite //:verification"
contains "$tmp/query_suite.out" "resolved_tests [//:scheduler_unit, //:queue_unit, //:emulator_smoke]"
run_capture query_suite_json "$qstar" query //:verification --format json
contains "$tmp/query_suite_json.out" '"schema":"qstar-query-v1"'
contains "$tmp/query_suite_json.out" '"kind":"test_suite"'
run_capture query_target "$qstar" query //:scheduler_unit
contains "$tmp/query_target.out" "direct_test_suites [//:host_units]"
contains "$tmp/query_target.out" "test_suites [//:host_units, //:verification]"
run_capture query_target_json "$qstar" query //:scheduler_unit --format json
contains "$tmp/query_target_json.out" '"kind":"target"'
contains "$tmp/query_target_json.out" '"test_suites":["//:host_units","//:verification"]'

run_capture stella_suite "$qstar" -B build/stella -G stella test --suite //:verification
contains "$tmp/stella_suite.out" "resolved_test_members [//:scheduler_unit, //:queue_unit, //:emulator_smoke]"
contains "$tmp/stella_suite.out" "test_member label=//:emulator_smoke kind=run_target"
contains "$tmp/stella_suite.out" "PROBE_OK emulator"
contains "$tmp/stella_suite.out" "status ok"

run_capture stella_tag "$qstar" -B build/stella -G stella test --tag host
contains "$tmp/stella_tag.out" "resolved_test_members [//:scheduler_unit, //:queue_unit]"
not_contains "$tmp/stella_tag.out" "PROBE_OK emulator"

run_capture stella_exclude "$qstar" -B build/stella -G stella test \
	--suite //:verification --exclude-tag emulator
contains "$tmp/stella_exclude.out" "resolved_test_members [//:scheduler_unit, //:queue_unit]"
not_contains "$tmp/stella_exclude.out" "PROBE_OK emulator"

run_failure implicit_manual "$qstar" -B build/stella test --tag hardware
contains "$tmp/implicit_manual.err" "no test suite members matched"
run_capture explicit_manual "$qstar" -B build/stella test --suite //:hardware_manual
contains "$tmp/explicit_manual.out" "resolved_test_members [//:hardware_probe]"
contains "$tmp/explicit_manual.out" "PROBE_OK hardware"

run_capture repeated_union "$qstar" -B build/stella test \
	--suite //:host_units --suite //:verification --tag emulator
contains "$tmp/repeated_union.out" "resolved_test_members [//:scheduler_unit, //:queue_unit, //:emulator_smoke]"

run_capture legacy_positional "$qstar" -B build/stella test //:scheduler_unit
not_contains "$tmp/legacy_positional.out" "resolved_test_members"
run_capture legacy_all "$qstar" -B build/stella test
contains "$tmp/legacy_all.out" "test_result label=//:scheduler_unit status=pass exit=0"
contains "$tmp/legacy_all.out" "test_result label=//:queue_unit status=pass exit=0"
not_contains "$tmp/legacy_all.out" "PROBE_OK emulator"

if command -v ninja >/dev/null 2>&1; then
	run_capture ninja_suite "$qstar" -B build/ninja -G ninja test \
		--suite //:verification
	contains "$tmp/ninja_suite.out" "backend ninja"
	contains "$tmp/ninja_suite.out" "resolved_test_members [//:scheduler_unit, //:queue_unit, //:emulator_smoke]"
	contains "$tmp/ninja_suite.out" "test_member label=//:emulator_smoke kind=run_target backend=ninja"
	contains "$tmp/ninja_suite.out" "PROBE_OK emulator"
fi

run_failure mixed_selection "$qstar" test //:scheduler_unit --suite //:host_units
contains "$tmp/mixed_selection.err" "usage: qstar"
run_failure unknown_suite "$qstar" test --suite //:missing
contains "$tmp/unknown_suite.err" "unknown test_suite label '//:missing'"
run_failure empty_tag "$qstar" test --tag=
contains "$tmp/empty_tag.err" "test tag filters must be non-empty"

for case_name in cycle duplicate-member duplicate-tag unknown-field unknown-member wrong-kind; do
	run_failure "$case_name" "$qstar" --file "negative/$case_name.lua" check
done
contains "$tmp/cycle.err" "test_suite cycle includes"
contains "$tmp/duplicate-member.err" "duplicate member '//negative:unit'"
contains "$tmp/duplicate-tag.err" "duplicate tag 'host'"
contains "$tmp/unknown-field.err" "unknown field 'tagz'"
contains "$tmp/unknown-member.err" "references unknown label '//:missing'"
contains "$tmp/wrong-kind.err" "must be qstar.test, run_target, or another test_suite"

run_failure module_declaration "$qstar" --file negative/module/qstar.lua check
contains "$tmp/module_declaration.err" "qstar.test_suite is forbidden inside .qsm module"

printf 'qstar-composable-test-suites: passed\n'
