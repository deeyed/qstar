qstar.project {
  name = "toolchain-app-corpus",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.toolset "fake_clang" {
  tools = {
    c = qstar.cli {"tools/fake-clang.sh"},
    cxx = qstar.cli {"tools/fake-clang.sh"},
    asm = qstar.cli {"tools/fake-clang.sh"},
    archive = qstar.cli {"ar"},
    link = qstar.cli {"tools/fake-link.sh"},
  },
  response_files = "on",
  response_style = "posix",
}

qstar.config "module_c" {
  toolset = "//:fake_clang",
  lang = {
    c = {
      public_include_dirs = {
        "include",
      },
      system_include_dirs = {
        "sysroot/include",
      },
      compile_options = {
        "-std=c23",
        "-Wall",
        "-Wextra",
        "-Werror",
      },
    },
  },
}

qstar.executable "module_app" {
  configs = {
    "//:module_c",
  },
  sources = {
    "startup/start.S",
    "src/module.c",
  },
  link_options = {
    "-T",
    "linker/module.ld",
    "--defsym=__module_entry=0x1000",
  },
  link_inputs = {
    "linker/module.ld",
  },
  lang = {
    asm = {
      include_dirs = {
        "startup",
      },
      compile_options = {
        "-D__QSTAR_TOOLCHAIN_APP__=1",
      },
      preprocess = true,
    },
  },
}

qstar.custom_target "package_blob" {
  inputs = {
    qstar.target_file("//:module_app"),
  },
  outputs = {
    qstar.output("build/qstar/generated/package.bin", {
      group = "packages",
    }),
  },
  command = qstar.cli {
    "tools/fake-objcopy.sh",
    "-O",
    "binary",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.group "all" {
  deps = {
    "//:module_app",
  },
}
