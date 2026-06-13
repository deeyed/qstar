qstar.project {
  name = "generated-ninja-corpus",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.configure_file "cfg" {
  output = qstar.output("build/qstar/generated/config.h"),
  defines = {"QSTAR_GENERATED_VALUE=23"},
  description = qstar.status("Configuring generated config.h"),
}

qstar.custom_target "make_value" {
  inputs = {qstar.target_file("//:cfg")},
  outputs = {qstar.output("build/qstar/generated/value.c")},
  command = qstar.cli {
    "tools/gen-value.sh",
    qstar.target_file("//:cfg"),
    qstar.output(0),
  },
  description = qstar.status("Generating generated value.c"),
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("build/qstar/generated/value.c"),
  },
  lang = {
    c = {
      private_headers = {qstar.output("build/qstar/generated/config.h")},
      include_dirs = {"build/qstar/generated"},
      compile_options = {"-Wall", "-Wextra"},
    },
  },
}

qstar.test "unit" {
  sources = {
    "tests/unit.c",
    qstar.output("build/qstar/generated/value.c"),
  },
  lang = {
    c = {
      private_headers = {qstar.output("build/qstar/generated/config.h")},
      include_dirs = {"build/qstar/generated"},
      compile_options = {"-Wall", "-Wextra"},
    },
  },
}

qstar.run_target "smoke" {
  deps = {"//:app"},
  command = qstar.cli {qstar.target_file("//:app")},
  timeout = 3,
  marker = "GENERATED-OK",
  description = qstar.status("Running generated smoke"),
}

qstar.stage "bundle" {
  root = "stage/bundle",
  files = {
    qstar.stage_file(qstar.target_file("//:app"), "bin/app"),
    qstar.stage_file(qstar.target_file("//:make_value"), "share/value.c"),
  },
}

qstar.group "all" {
  deps = {
    "//:app",
    "//:unit",
    "//:smoke",
  },
}
