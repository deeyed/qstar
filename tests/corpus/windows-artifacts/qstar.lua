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
    "//:profile_tool=profile_tool.exe",
    "//:profile_core=profile_core.lib",
    "//:plugin=plugin.dll",
  },
}

qstar.profile "windows-msvc-fake" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "tools/fake-clang-cl",
  cxx = "tools/fake-clang-cl",
  ar = "tools/fake-lib",
  linker = "tools/fake-clang-cl",
  response_files = "on",
  response_style = "msvc",
  artifact_names = {
    "//:tool=tool.exe",
    "//:core=core.lib",
    "//:profile_tool=profile_tool.exe",
    "//:profile_core=profile_core.lib",
    "//:plugin=plugin.dll",
  },
}

qstar.config "windows_c" {
  lang = {
    c = {
      compile_options = {
        "/W4",
        "/DQSTAR_WINDOWS_ARTIFACT=alpha beta",
        "/DQSTAR_WINDOWS_PATH=C:\\Program Files\\QStar\\Include",
        "/DQSTAR_WINDOWS_QUOTE=\"artifact\"",
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

qstar.executable "profile_tool" {
  configs = {
    "//:windows_c",
  },
  sources = {
    "src/profile_tool.c",
  },
  libs = {
    "kernel32",
  },
}

qstar.staticlib "profile_core" {
  configs = {
    "//:windows_c",
  },
  sources = {
    "src/profile_core.c",
  },
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
    qstar.stage_file(qstar.target_file("//:profile_tool"), "bin/profile_tool.exe"),
    qstar.stage_file(qstar.target_file("//:profile_core"), "lib/profile_core.lib"),
  },
}

qstar.group "all" {
  deps = {
    "//:tool",
    "//:core",
    "//:profile_tool",
    "//:profile_core",
  },
}
