local zig = qstar.use_language("zig")

qstar.project {
  name = "zig-static-consumer",
  version = "0.1",
  root = ".",
  build_dir = "build/qstar",
}

qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    zig = zig.tools {
      compiler = qstar.cli {"zig"},
    },
  },
}

qstar.config "native" {
  toolset = "//:host",
  lang = {
    c = {
      compile_options = {"-std=c11", "-Wall", "-Wextra"},
    },
    zig = zig.options {
      target = "native",
      optimize = "Debug",
    },
  },
}

qstar.staticlib "zig_core" {
  configs = {"//:native"},
  sources = {
    "src/zig_core.zig",
  },
}

qstar.executable "consumer" {
  configs = {"//:native"},
  sources = {
    "src/consumer.c",
  },
  deps = {
    "//:zig_core",
  },
}

qstar.run_target "smoke" {
  deps = {
    "//:consumer",
  },
  command = qstar.cli {
    qstar.target_file("//:consumer"),
  },
  expect = {
    contains = "zig-value=88",
  },
}

