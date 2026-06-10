qstar.project {
  name = "c-only",
  version = "0.1.0",
  root = ".",
}

qstar.staticlib "core" {
  sources = {"src/core.c"},
  public_headers = {"include/corpus.h"},
  lang = {
    c = {
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
