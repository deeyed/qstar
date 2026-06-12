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
