qstar.config_header "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"QSTAR_PROJECT_VALUE=17"},
}

qstar.genrule "generated_source" {
  tool = "tools/gen-value.sh",
  outputs = {qstar.output("generated/value.c")},
  args = {"generated/value.c"},
}

qstar.exe "app" {
  sources = {"src/main.c", qstar.output("generated/value.c")},
  private_headers = {qstar.output("generated/config.h")},
  include_dirs = {"generated"},
}
