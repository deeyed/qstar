local zig = qstar.use_language("zig")
local zig_target = "native"
local zig_macos_min_version = ""

if qstar.host.os == "macos" then
  zig_target = qstar.host.arch .. "-macos"
  zig_macos_min_version = "11.0"
end

qstar.project {
  name = "zig-executable",
  version = "0.1",
  root = ".",
  build_dir = "build/qstar",
}

qstar.toolset "host" {
  tools = {
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
    zig = zig.options {
      target = zig_target,
      optimize = "Debug",
      macos_min_version = zig_macos_min_version,
    },
  },
}

qstar.executable "app" {
  configs = {"//:native"},
  sources = {
    "src/main.zig",
  },
}

qstar.run_target "smoke" {
  deps = {
    "//:app",
  },
  command = qstar.cli {
    qstar.target_file("//:app"),
  },
  expect = {
    contains = "zig-exe-ok",
  },
}
