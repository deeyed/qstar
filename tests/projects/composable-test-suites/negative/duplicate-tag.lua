qstar.test "unit" {
  sources = {"tests/scheduler_unit.c"},
}

qstar.test_suite "duplicate" {
  tests = {":unit"},
  tags = {"host", "host"},
}
