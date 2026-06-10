qstar.project {
  name = "generated-config",
  version = "0.1.0",
  root = ".",
}

qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"QSTAR_PROJECT_VALUE=17"},
}

qstar.custom_target "generated_source" {
  outputs = {qstar.output("generated/value.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}

qstar.executable "app" {
  sources = {"src/main.c", qstar.output("generated/value.c")},
  private_headers = {qstar.output("generated/config.h")},
  lang = {
    c = {
      include_dirs = {"generated"},
    },
  },
}
