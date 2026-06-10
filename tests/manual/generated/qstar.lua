qstar.project {
  name = "generated",
  version = "0.1.0",
  root = ".",
}

qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"APP_VALUE=42", "APP_FEATURE"},
}

qstar.custom_target "generated_value" {
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
