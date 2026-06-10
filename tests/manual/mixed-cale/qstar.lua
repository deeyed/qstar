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
