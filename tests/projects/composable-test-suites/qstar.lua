qstar.project {
  name = "composable-test-suites",
  version = "0.1.0",
  root = ".",
}

qstar.test "scheduler_unit" {
  sources = {
    "tests/scheduler_unit.c",
  },
}

qstar.test "queue_unit" {
  sources = {
    "tests/queue_unit.c",
  },
}

qstar.run_target "emulator_smoke" {
  command = qstar.cli {"tools/probe.sh", "emulator"},
  timeout = 5,
  expect = {
    contains = "PROBE_OK emulator",
  },
  description = qstar.status("Running the emulator-classified project probe"),
}

qstar.run_target "hardware_probe" {
  command = qstar.cli {"tools/probe.sh", "hardware"},
  timeout = 5,
  expect = {
    contains = "PROBE_OK hardware",
  },
  description = qstar.status("Running the manually selected hardware-classified probe"),
}

qstar.test_suite "host_units" {
  tests = {
    "//:scheduler_unit",
    "//:queue_unit",
  },
  tags = {
    "host",
    "fast",
  },
  description = qstar.status("Host-classified unit test targets"),
}

qstar.test_suite "emulator_runs" {
  tests = {
    "//:emulator_smoke",
  },
  tags = {
    "emulator",
    "smoke",
  },
  description = qstar.status("Emulator-classified run target"),
}

qstar.test_suite "verification" {
  tests = {
    "//:host_units",
    "//:emulator_runs",
  },
  tags = {
    "verification",
  },
  description = qstar.status("Nested verification suite"),
}

qstar.test_suite "hardware_manual" {
  tests = {
    "//:hardware_probe",
  },
  tags = {
    "hardware",
    "slow",
  },
  description = qstar.status("Manual hardware-classified project probe"),
  manual = true,
}
