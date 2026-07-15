qstar.test_suite "first" {
  tests = {":second"},
}

qstar.test_suite "second" {
  tests = {":first"},
}
