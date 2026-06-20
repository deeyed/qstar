qstar.project {
  name = "c-app-ninja-corpus",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  compile_commands = "build",
}

qstar.config "c_warnings" {
  lang = {
    c = {
      compile_options = {"-Wall", "-Wextra"},
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:c_warnings"},
  sources = {"src/core.c"},
  lang = {
    c = {
      public_headers = {"include/corpus.h"},
      public_include_dirs = {"include"},
    },
  },
}

qstar.executable "app" {
  configs = {"//:c_warnings"},
  sources = {"src/main.c"},
  deps = {"//:core"},
}

qstar.test "unit" {
  configs = {"//:c_warnings"},
  sources = {"tests/unit.c"},
  deps = {"//:core"},
}

qstar.group "all" {
  deps = {
    "//:app",
    "//:unit",
  },
}

qstar.stage "install_layout" {
  root = "build/qstar/stage/install",
  files = {
    qstar.stage_file(qstar.target_file("//:core"), "lib/libcore.a"),
    qstar.stage_file("include/corpus.h", "include/corpus.h"),
  },
}

qstar.command "install" {
  options = {
    out = qstar.param.path {
      default = "exports/install",
    },
  },
  steps = {
    qstar.step.export_stage("//:install_layout", {
      to = qstar.param("out"),
    }),
  },
}
