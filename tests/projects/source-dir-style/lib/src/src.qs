qstar.staticlib "core" {
  sources = {"lib/src/core.c"},
  public_headers = {"lib/src/include/core.h"},
  lang = {
    c = {
      public_include_dirs = {"lib/src/include"},
    },
  },
}
