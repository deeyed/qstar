#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
QSTAR=${QSTAR_TEST_QSTAR:-$ROOT/build/bin/qstar}
FIXTURE=$ROOT/tests/projects/test-resources-results
TMP=${TMPDIR:-/tmp}/qstar-test-resources-results-$$

fail()
{
  echo "test-resources-results: $*" >&2
  exit 1
}

rm -rf "$TMP"
mkdir -p "$TMP"
cp -R "$FIXTURE/." "$TMP/"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

(
  cd "$TMP"
  "$QSTAR" check //... > check.out
  "$QSTAR" test --suite //:successful --jobs 4 \
    --report-json results/stella.json \
    --output-junit results/stella.xml > stella.out
)

grep -q 'test_resource event=acquire test=//:resource_one resource=shared.slot amount=1 used=1 capacity=1' "$TMP/stella.out" ||
  fail "resource_one did not acquire the generic resource"
grep -q 'test_resource event=acquire test=//:resource_two resource=shared.slot amount=1 used=1 capacity=1' "$TMP/stella.out" ||
  fail "resource_two did not wait for the generic resource"
if grep -q 'resource=shared.slot.*used=2' "$TMP/stella.out"; then
  fail "resource capacity was exceeded"
fi
grep -q 'test_retry label=//:retry status=fail completed_attempt=1 next_attempt=2' "$TMP/stella.out" ||
  fail "retry transition was not recorded"
grep -q 'test_result label=//:retry status=pass exit=0 attempts=2' "$TMP/stella.out" ||
  fail "retry did not reach pass"
grep -q 'test_result label=//:manual status=pass' "$TMP/stella.out" ||
  fail "explicit suite selection did not run manual test"
grep -q 'test_result label=//:declared_skip status=skip' "$TMP/stella.out" ||
  fail "declarative skip was not recorded"
grep -q '"schema":"qstar-test-results-v1"' "$TMP/results/stella.json" ||
  fail "JSON result schema is missing"
grep -q '"label":"//:retry","status":"pass","attempts":2' "$TMP/results/stella.json" ||
  fail "JSON retry result is wrong"
grep -q '"action_id":"//:retry:test:test:2"' "$TMP/results/stella.json" ||
  fail "JSON result is not linked to the test action log"
grep -q '<testsuite name="qstar" tests="5" failures="0" errors="0" skipped="1">' "$TMP/results/stella.xml" ||
  fail "JUnit summary is wrong"

(
  cd "$TMP"
  "$QSTAR" action-log '//:retry:test:test:2' > action-log.out
  "$QSTAR" replay '//:retry:test:test:2' > replay.out
)
grep -q 'qstar-action-log v2' "$TMP/action-log.out" ||
  fail "test action-log is not connected"
grep -q 'action //:retry:test:test:2' "$TMP/replay.out" ||
  fail "test replay is not connected"

rm -f "$TMP/retry.flag"
(
  cd "$TMP"
  "$QSTAR" -G ninja test --suite //:successful --jobs 4 \
    --report-json results/ninja.json \
    --output-junit results/ninja.xml > ninja.out
)
grep -q '"backend":"ninja"' "$TMP/results/ninja.json" ||
  fail "Ninja report backend is wrong"
grep -q 'test_result label=//:retry status=pass exit=0 attempts=2' "$TMP/ninja.out" ||
  fail "Ninja retry behavior differs"
if grep -q 'resource=shared.slot.*used=2' "$TMP/ninja.out"; then
  fail "Ninja execution exceeded resource capacity"
fi
(
  cd "$TMP"
  "$QSTAR" -G ninja action-log '//:retry:test:test:2' > ninja-action-log.out
  "$QSTAR" -G ninja replay '//:retry:test:test:2' > ninja-replay.out
)
grep -q 'backend=ninja' "$TMP/ninja-action-log.out" ||
  fail "Ninja test action-log backend is wrong"
grep -q 'action //:retry:test:test:2' "$TMP/ninja-replay.out" ||
  fail "Ninja test replay is not connected"

rm -f "$TMP/retry.flag"
if (
  cd "$TMP"
  "$QSTAR" test --jobs 4 --report-json results/failures.json \
    --output-junit results/failures.xml > failures.out 2> failures.err
); then
  fail "timeout and cleanup failure run unexpectedly succeeded"
fi
grep -q 'test_result label=//:timeout status=timeout' "$TMP/failures.out" ||
  fail "timeout result is missing"
grep -q 'test_result label=//:cleanup_failure status=error' "$TMP/failures.out" ||
  fail "cleanup failure was not classified as error"
grep -q 'test_result label=//:manual status=skip attempts=0 reason=manual' "$TMP/failures.out" ||
  fail "manual test was not excluded from automatic selection"
grep -q '"status":"timeout"' "$TMP/results/failures.json" ||
  fail "timeout is missing from JSON"
grep -q '"status":"error"' "$TMP/results/failures.json" ||
  fail "cleanup error is missing from JSON"
grep -q 'errors="2"' "$TMP/results/failures.xml" ||
  fail "JUnit error count is wrong"

mkdir -p "$TMP/bad"
cp "$FIXTURE/qstar.lua" "$FIXTURE/pass.c" "$TMP/bad/"
cp -R "$FIXTURE/hooks" "$TMP/bad/"
printf '\nqstar.test "bad" { sources = {"pass.c"}, resources = { missing = 1 } }\n' >> "$TMP/bad/qstar.lua"
if (cd "$TMP/bad" && "$QSTAR" check //... > bad.out 2> bad.err); then
  fail "unknown resource request unexpectedly passed"
fi
grep -q "requests unknown resource 'missing'" "$TMP/bad/bad.err" ||
  fail "unknown resource diagnostic is missing"

mkdir -p "$TMP/bad-capacity"
cp "$FIXTURE/qstar.lua" "$FIXTURE/pass.c" "$TMP/bad-capacity/"
cp -R "$FIXTURE/hooks" "$TMP/bad-capacity/"
printf '\nqstar.test "bad" { sources = {"pass.c"}, resources = { ["shared.slot"] = 2 } }\n' >> "$TMP/bad-capacity/qstar.lua"
if (cd "$TMP/bad-capacity" && "$QSTAR" check //... > bad.out 2> bad.err); then
  fail "resource request above capacity unexpectedly passed"
fi
grep -q "request 2 exceeds capacity 1" "$TMP/bad-capacity/bad.err" ||
  fail "resource capacity diagnostic is missing"

mkdir -p "$TMP/bad-retry"
cp "$FIXTURE/qstar.lua" "$FIXTURE/pass.c" "$TMP/bad-retry/"
cp -R "$FIXTURE/hooks" "$TMP/bad-retry/"
printf '\nqstar.test "bad" { sources = {"pass.c"}, retry = { count = 1, on = {"skip"} } }\n' >> "$TMP/bad-retry/qstar.lua"
if (cd "$TMP/bad-retry" && "$QSTAR" check //... > bad.out 2> bad.err); then
  fail "unsupported retry state unexpectedly passed"
fi
grep -q "retry.on item 'skip' must be fail, error, or timeout" "$TMP/bad-retry/bad.err" ||
  fail "retry state diagnostic is missing"

mkdir -p "$TMP/bad-field"
cp "$FIXTURE/pass.c" "$TMP/bad-field/"
printf '%s\n' 'qstar.project { name = "bad-field" }' \
  'qstar.test_resource "slot" { capacity = 1, device = "builtin" }' \
  'qstar.test "bad" { sources = {"pass.c"} }' > "$TMP/bad-field/qstar.lua"
if (cd "$TMP/bad-field" && "$QSTAR" check //... > bad.out 2> bad.err); then
  fail "unknown test_resource field unexpectedly passed"
fi
grep -q "qstar.test_resource declaration 'slot': unknown field 'device'" "$TMP/bad-field/bad.err" ||
  fail "strict test_resource schema diagnostic is missing"

echo "test-resources-results: ok"
