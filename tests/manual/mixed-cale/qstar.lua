qstar.project {
  name = "mixed-cale",
  version = "0.1.0",
  root = ".",
}

qstar.executable "mixed" {
  toolchain = "cale",
  sources = {"src/main.c", "src/plugin.cale"},
  lang = {
    cale = {
      profile = "safe",
      compile_options = {},
      hcl_include_dirs = {},
    },
  },
}
