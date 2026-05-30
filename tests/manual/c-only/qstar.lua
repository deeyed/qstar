qstar.staticlib "core" {
  sources = {"src/core.c"},
  public_headers = {"include/corpus.h"},
  public_include_dirs = {"include"},
}

qstar.exe "app" {
  sources = {"src/main.c"},
  deps = {"//:core"},
}

qstar.test "unit" {
  sources = {"tests/unit.c"},
  deps = {"//:core"},
}
