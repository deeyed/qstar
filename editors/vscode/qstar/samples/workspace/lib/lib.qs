qstar.staticlib "core" {
  sources = {
    "lib/src/core.c",
  },
  public_headers = {
    "lib/include/sample/core.h",
  },
  public_include_dirs = {
    "lib/include",
  },
  visibility = {
    "//app:...",
  },
}
