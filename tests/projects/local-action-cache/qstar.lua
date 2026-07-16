qstar.project {
  name = "local-action-cache",
  version = "0.1.0",
  root = ".",
  action_cache = "local",
}

qstar.toolset "fake_tools" {
  allow_absolute_tools = "on",
  tools = {
    c = {
      compiler = qstar.cli {"tools/fake-cc"},
    },
  },
  path_tools = {
    "fake-transform",
    "qemu-system-probe",
  },
}

qstar.objectlib "objects" {
  toolset = "//:fake_tools",
  sources = {
    "src/input.c",
  },
}

qstar.config "noncache_policy" {
  cacheable = false,
}

qstar.objectlib "uncached_objects" {
  toolset = "//:fake_tools",
  configs = {
    "//:noncache_policy",
  },
  sources = {
    "src/uncached.c",
  },
}

qstar.transform "cached_artifact" {
  toolset = "//:fake_tools",
  input = "fixtures/payload.txt",
  output = qstar.output("generated/cached/payload.txt"),
  command = qstar.cli {
    "fake-transform",
    "--count",
    "tools/transform.count",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.transform "uncached_artifact" {
  toolset = "//:fake_tools",
  input = "fixtures/payload.txt",
  output = qstar.output("generated/uncached/payload.txt"),
  command = qstar.cli {
    "fake-transform",
    "--count",
    "tools/uncached.count",
    qstar.input(0),
    qstar.output(0),
  },
  cacheable = false,
}

qstar.custom_target "external_runtime" {
  toolset = "//:fake_tools",
  outputs = {
    qstar.output("generated/external/probe.txt"),
  },
  command = qstar.cli {
    "qemu-system-probe",
    "--count",
    "tools/external.count",
    qstar.output(0),
  },
}
