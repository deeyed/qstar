#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-generic-dsl-backend-seal.$$
artifact_dir=${QSTAR_GENERIC_DSL_SEAL_ARTIFACT_DIR:-}
generated=tests/corpus/generated
object_bridge=tests/projects/object-artifact-bridge
medium_raw="$tmp/medium.out"
medium_summary="$tmp/medium-summary.out"
medium_summary_md="$tmp/medium-summary.md"

fail() {
	printf 'qstar-generic-dsl-backend-seal: %s\n' "$1" >&2
	exit 1
}

contains() {
	file=$1
	pattern=$2
	grep -F -q -- "$pattern" "$file" ||
		fail "missing pattern '$pattern' in $file"
}

run_capture() {
	name=$1
	shift
	if ! "$@" > "$tmp/$name.out" 2> "$tmp/$name.err"; then
		printf 'qstar-generic-dsl-backend-seal: command failed: %s\n' "$name" >&2
		if [ -f "$tmp/$name.out" ]; then
			printf '%s\n' "--- $name.out ---" >&2
			tail -n 80 "$tmp/$name.out" >&2 || true
		fi
		if [ -f "$tmp/$name.err" ]; then
			printf '%s\n' "--- $name.err ---" >&2
			tail -n 80 "$tmp/$name.err" >&2 || true
		fi
		exit 1
	fi
}

cleanup() {
	rm -rf "$tmp"
	rm -rf "$generated/build/stella-generic-seal" "$generated/stage/generic-seal"
	rm -rf "$generated/build/qstar"
	rm -rf "$object_bridge/build/stella-generic-seal" "$object_bridge/stage/generic-seal"
	rm -rf "$object_bridge/build/qstar"
}

rm -rf "$tmp"
mkdir -p "$tmp"
trap cleanup EXIT HUP INT TERM
rm -rf "$generated/build/stella-generic-seal" "$generated/stage/generic-seal"
rm -rf "$generated/build/qstar"
rm -rf "$object_bridge/build/stella-generic-seal" "$object_bridge/stage/generic-seal"
rm -rf "$object_bridge/build/qstar"

command -v ninja >/dev/null 2>&1 ||
	fail "ninja is required for the generic DSL backend seal"

printf 'qstar-generic-dsl-backend-seal: qstar=%s\n' "$qstar"

run_capture self_check "$qstar" --file qstar.lua check
contains "$tmp/self_check.out" "status ok"

QSTAR_TEST_QSTAR="$qstar" sh tests/self-host.sh > "$tmp/self-host.out" 2> "$tmp/self-host.err" ||
	fail "self-host gate failed; see $tmp/self-host.out and $tmp/self-host.err"
contains "$tmp/self-host.out" "qstar-self-host: passed"
printf 'qstar-generic-dsl-backend-seal: self_host=ok\n'

run_capture generated_stella \
	"$qstar" --file "$generated/qstar.lua" -B build/stella-generic-seal -G stella build //:all --progress off
contains "$tmp/generated_stella.out" "status ok"
test -x "$generated/build/stella-generic-seal/out/___app/app" ||
	fail "generated Stella app missing"
"$generated/build/stella-generic-seal/out/___app/app" > "$tmp/generated-app.out"
contains "$tmp/generated-app.out" "GENERATED-OK"
test -f "$generated/build/qstar/generated/config.h" ||
	fail "generated Stella config header missing"
test -f "$generated/build/qstar/generated/value.c" ||
	fail "generated Stella source missing"
printf 'qstar-generic-dsl-backend-seal: generated_stella=ok\n'

case "$(uname -s)" in
	Darwin) object_shared_artifact="build/stella-generic-seal/out/___objc_plugin/libobjc_plugin.dylib" ;;
	*) object_shared_artifact="build/stella-generic-seal/out/___objc_plugin/libobjc_plugin.so" ;;
esac
run_capture object_bridge_stella \
	"$qstar" --file "$object_bridge/qstar.lua" -B build/stella-generic-seal -G stella build //:all --progress off
contains "$tmp/object_bridge_stella.out" "status ok"
test -f "$object_bridge/build/qstar/generated/objc/AppDelegate.o" ||
	fail "object bridge Stella generated object missing"
test -x "$object_bridge/build/stella-generic-seal/out/___app/app" ||
	fail "object bridge Stella app missing"
test -f "$object_bridge/build/stella-generic-seal/out/___objc_static/libobjc_static.a" ||
	fail "object bridge Stella staticlib missing"
test -f "$object_bridge/$object_shared_artifact" ||
	fail "object bridge Stella sharedlib missing"
"$object_bridge/build/stella-generic-seal/out/___app/app" > "$tmp/object-bridge-app.out"
printf 'qstar-generic-dsl-backend-seal: object_bridge_stella=ok\n'

QSTAR_TEST_QSTAR="$qstar" sh tests/ninja-backend-parity.sh > "$tmp/ninja-backend-parity.out" 2> "$tmp/ninja-backend-parity.err" ||
	fail "Ninja backend parity gate failed; see $tmp/ninja-backend-parity.out and $tmp/ninja-backend-parity.err"
contains "$tmp/ninja-backend-parity.out" "qstar-ninja-backend-parity: passed"
printf 'qstar-generic-dsl-backend-seal: ninja_backend_parity=ok\n'

QSTAR_TEST_QSTAR="$qstar" sh tests/medium-project-performance.sh > "$medium_raw" 2> "$tmp/medium.err" ||
	fail "medium performance gate failed; see $medium_raw and $tmp/medium.err"
contains "$medium_raw" "medium_project_gate backend=stella phase=clean"
contains "$medium_raw" "medium_project_gate backend=stella phase=noop"
contains "$medium_raw" "medium_project_gate backend=stella phase=incremental"
contains "$medium_raw" "medium_project_gate backend=ninja phase=clean"
contains "$medium_raw" "medium_project_gate backend=ninja phase=noop"
contains "$medium_raw" "medium_project_gate backend=ninja phase=incremental"
contains "$medium_raw" "medium_project_gate status=ok"
tools/perf-summary.sh \
	--warn-ratio-x100 "${QSTAR_GENERIC_DSL_WARN_RATIO_X100:-200}" \
	--warn-slack-ms "${QSTAR_GENERIC_DSL_WARN_SLACK_MS:-250}" \
	--hard-ratio-x100 "${QSTAR_GENERIC_DSL_HARD_RATIO_X100:-250}" \
	--hard-slack-ms "${QSTAR_GENERIC_DSL_HARD_SLACK_MS:-500}" \
	--hard "$medium_raw" > "$medium_summary"
contains "$medium_summary" "perf_summary status=ok"
tools/perf-summary.sh --format markdown --label "Generic DSL backend seal" \
	"$medium_raw" > "$medium_summary_md"
printf 'qstar-generic-dsl-backend-seal: medium_performance=ok\n'

QSTAR_TEST_QSTAR="$qstar" sh tests/linux-validation.sh > "$tmp/linux-validation.out" 2> "$tmp/linux-validation.err" ||
	fail "Linux validation gate failed; see $tmp/linux-validation.out and $tmp/linux-validation.err"
contains "$tmp/linux-validation.out" "qstar-linux-validation: passed"
printf 'qstar-generic-dsl-backend-seal: linux_validation=ok\n'

if [ -n "$artifact_dir" ]; then
	mkdir -p "$artifact_dir"
	cp "$medium_raw" "$artifact_dir/generic-dsl-medium-perf.txt"
	cp "$medium_summary" "$artifact_dir/generic-dsl-medium-summary.txt"
	cp "$medium_summary_md" "$artifact_dir/generic-dsl-medium-summary.md"
	cp "$tmp/self-host.out" "$artifact_dir/generic-dsl-self-host.txt"
	cp "$tmp/ninja-backend-parity.out" "$artifact_dir/generic-dsl-ninja-backend-parity.txt"
	cp "$tmp/linux-validation.out" "$artifact_dir/generic-dsl-linux-validation.txt"
fi

printf 'qstar-generic-dsl-backend-seal: passed\n'
