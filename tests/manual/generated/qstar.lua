qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"APP_VALUE=42", "APP_FEATURE"},
}

qstar.custom_target "generated_value" {
  tool = "tools/gen-value.sh",
  outputs = {qstar.output("generated/value.c")},
  args = {"generated/value.c"},
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
