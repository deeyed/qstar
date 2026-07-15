qstar.test "unit" {
  sources = {"tests/scheduler_unit.c"},
}

qstar.test_suite "typo" {
  tests = {":unit"},
  tagz = {"host"},
}
