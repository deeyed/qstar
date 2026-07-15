qstar.project {
  name = "test-resources-results",
  build_dir = "build",
}

qstar.test_resource "shared.slot" {
  capacity = 1,
  description = qstar.status("Generic exclusive scheduler slot"),
}

qstar.test "resource_one" {
  sources = {"pass.c"},
  resources = { ["shared.slot"] = 1 },
  setup = qstar.cli {"sh", "hooks/setup.sh", "resource_one"},
  cleanup = qstar.cli {"sh", "hooks/cleanup.sh", "resource_one"},
}

qstar.test "resource_two" {
  sources = {"pass.c"},
  resources = { ["shared.slot"] = 1 },
  setup = qstar.cli {"sh", "hooks/setup.sh", "resource_two"},
  cleanup = qstar.cli {"sh", "hooks/cleanup.sh", "resource_two"},
}

qstar.test "retry" {
  sources = {"retry.c"},
  retry = {
    count = 1,
    on = {"fail"},
  },
}

qstar.test "manual" {
  sources = {"pass.c"},
  manual = true,
}

qstar.test "declared_skip" {
  sources = {"pass.c"},
  skip = qstar.status("Not enabled in this test environment"),
}

qstar.test "timeout" {
  sources = {"slow.c"},
  timeout = 1,
}

qstar.test "cleanup_failure" {
  sources = {"pass.c"},
  cleanup = qstar.cli {"sh", "hooks/cleanup-fail.sh"},
}

qstar.test_suite "successful" {
  tests = {
    "//:resource_one",
    "//:resource_two",
    "//:retry",
    "//:manual",
    "//:declared_skip",
  },
  tags = {"generic-resource"},
}
