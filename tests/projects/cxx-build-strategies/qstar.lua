qstar.project {
  name = "cxx-build-strategies",
  version = "0.1.0",
  root = ".",
  compile_commands = "build",
}

qstar.toolset "fake_clang" {
  allow_absolute_tools = "on",
  tools = {
    cxx = {
      compiler = qstar.cli {"tools/fake-clang++"},
    },
    link = qstar.cli {"tools/fake-clang++"},
  },
  path_tools = {"fake-pch-gen"},
}

qstar.custom_target "generated_pch_header" {
  toolset = "//:fake_clang",
  outputs = {qstar.output("generated/pch.hpp")},
  command = qstar.cli {"fake-pch-gen", qstar.output(0)},
}

qstar.executable "strategies" {
  toolset = "//:fake_clang",
  sources = {
    "src/math.cppm",
    "src/alpha.cpp",
    "src/beta.cpp",
    "src/module_use.cpp",
    "src/main.cpp",
  },
  lang = {
    cxx = {
      standard = "c++20",
      precompiled_header = "include/pch.hpp",
      unity = {
        enabled = true,
        batch_size = 2,
      },
      modules = {
        enabled = true,
      },
      include_dirs = {"include"},
    },
  },
}

qstar.executable "generated_pch" {
  toolset = "//:fake_clang",
  sources = {"src/pch_only.cpp"},
  lang = {
    cxx = {
      standard = "c++17",
      precompiled_header = "generated/pch.hpp",
    },
  },
}
