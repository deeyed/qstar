qstar.executable "app" {
  sources = {"tests/scheduler_unit.c"},
}

qstar.test_suite "wrong" {
  tests = {":app"},
}
