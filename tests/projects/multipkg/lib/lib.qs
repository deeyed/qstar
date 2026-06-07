qstar.staticlib "core" {
  sources = {"lib/src/core.c"},
  public_headers = {"lib/include/core.h"},
  public_include_dirs = {"lib/include"},
  visibility = {"//app:..."},
}
