local zig = qstar.use_language("zig")

local legacy = qstar.use_language("qstar/languages/legacy")

local pack = qstar.use_language("qstar/languages/pack")

qstar.project {
  name = "glp-v2-artifact-contract",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
}

qstar.toolset "host" {
  tools = {
    c = {
      compiler = qstar.cli {"cc"},
    },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    pack = pack.tools {
      compiler = qstar.cli {"tools/fake-pack.sh"},
    },
    legacy = legacy.tools {
      compiler = qstar.cli {"tools/fake-pack.sh"},
    },
    zig = zig.tools {
      compiler = qstar.cli {"tools/fake-pack.sh"},
    },
  },
  path_tools = {
    "sh",
  },
}

qstar.imported "prebuilt" {
  artifact_kind = "opaque",
  artifacts = {
    default = {
      {
        id = "link",
        role = "link",
        path = "vendor/prebuilt.link",
        primary = true,
      },
    },
  },
}

qstar.executable "mixed" {
  toolset = "//:host",
  deps = {
    "//:prebuilt",
  },
  link_inputs = {
    "vendor/extra.input",
  },
  link_options = {
    "--explicit-provider-option",
  },
  sources = {
    "src/native.c",
    "src/main.p2",
    "src/legacy.l1",
  },
}

qstar.executable "consumer" {
  toolset = "//:host",
  sources = {
    "src/consumer.p2",
  },
  deps = {
    "//:mixed",
  },
}

qstar.custom_target "inspect_runtime" {
  toolset = "//:host",
  inputs = {
    qstar.target_file("//:mixed"),
    qstar.target_file("//:mixed", {artifact = "metadata"}),
    qstar.target_file("//:mixed", {artifact = "resources"}),
    qstar.target_file("//:mixed", {artifact = "link"}),
  },
  outputs = {
    qstar.output("generated/inspected.txt"),
  },
  command = qstar.cli {
    "sh", "tools/inspect-pack.sh",
    qstar.input(0), qstar.input(1), qstar.input(2), qstar.input(3),
    qstar.output(0),
  },
}

qstar.group "all" {
  deps = {
    "//:mixed",
    "//:consumer",
    "//:inspect_runtime",
  },
}
