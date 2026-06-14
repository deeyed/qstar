qstar.project {
  name = "windows-artifacts-corpus",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.profile "windows-msvc" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "clang-cl",
  cxx = "clang-cl",
  linker = "clang-cl",
  response_files = "on",
  response_style = "msvc",
  artifact_names = {
    "//:tool=tool.exe",
    "//:core=core.lib",
    "//:plugin=plugin.dll",
  },
}

qstar.config "windows_c" {
  lang = {
    c = {
      compile_options = {
        "/W4",
      },
    },
  },
}

qstar.executable "tool" {
  configs = {
    "//:windows_c",
  },
  sources = {
    "src/main.c",
  },
  artifact_name = "tool.exe",
  libs = {
    "kernel32",
  },
}

qstar.staticlib "core" {
  configs = {
    "//:windows_c",
  },
  sources = {
    "src/core.c",
  },
  artifact_name = "core.lib",
}

qstar.sharedlib "plugin" {
  configs = {
    "//:windows_c",
  },
  sources = {
    "src/plugin.c",
  },
  artifact_name = "plugin.dll",
}

qstar.stage "layout" {
  root = "build/qstar/stage/windows-layout",
  files = {
    qstar.stage_file(qstar.target_file("//:tool"), "bin/tool.exe"),
    qstar.stage_file(qstar.target_file("//:core"), "lib/core.lib"),
  },
}

qstar.group "all" {
  deps = {
    "//:tool",
    "//:core",
  },
}
