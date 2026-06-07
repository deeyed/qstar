qstar.exe "app" {
  sources = {
    "app/src/main.c",
  },
  deps = {
    "//lib:core",
  },
}
