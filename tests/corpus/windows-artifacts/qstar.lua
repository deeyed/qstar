qstar.project {
  name = "windows-artifacts-corpus",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.toolset "windows_fake" {
  tools = {
    c = { compiler = qstar.cli {"tools/fake-clang-cl.sh"} },
    cxx = { compiler = qstar.cli {"tools/fake-clang-cl.sh"} },
    asm = { compiler = qstar.cli {"tools/fake-clang-cl.sh"} },
    archive = qstar.cli {"tools/fake-lib.sh"},
    link = qstar.cli {"tools/fake-clang-cl.sh"},
  },
  response_files = "on",
  response_style = "msvc",
}

qstar.config "windows_c" {
  toolset = "//:windows_fake",
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

qstar.executable "named_tool" {
  configs = {
    "//:windows_c",
  },
  sources = {
    "src/context_tool.c",
  },
  artifact_name = "named_tool.exe",
  libs = {
    "kernel32",
  },
}

qstar.staticlib "named_core" {
  configs = {
    "//:windows_c",
  },
  sources = {
    "src/context_core.c",
  },
  artifact_name = "named_core.lib",
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
    qstar.stage_file(qstar.target_file("//:named_tool"), "bin/named_tool.exe"),
    qstar.stage_file(qstar.target_file("//:named_core"), "lib/named_core.lib"),
  },
}

qstar.group "all" {
  deps = {
    "//:tool",
    "//:core",
    "//:named_tool",
    "//:named_core",
  },
}
