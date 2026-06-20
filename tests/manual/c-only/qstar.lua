qstar.project {
  name = "c-only",
  version = "0.1.0",
  root = ".",
}

qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = {
      public_headers = {"include/corpus.h"},
      public_include_dirs = {"include"},
    },
  },
}

qstar.executable "app" {
  sources = {"src/main.c"},
  deps = {"//:core"},
}

qstar.test "unit" {
  sources = {"tests/unit.c"},
  deps = {"//:core"},
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
