qstar.project {
  name = "freestanding-corpus",
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

qstar.config "kernel_c" {
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
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-mgeneral-regs-only",
        "-mcpu=cortex-a76",
        "-mabi=lp64",
        "-Wall",
        "-Wextra",
        "-Werror",
      },
    },
  },
}

qstar.executable "kernel" {
  configs = {
    "//:kernel_c",
  },
  sources = {
    "boot/start.S",
    "src/kernel.c",
  },
  link_options = {
    "-nostdlib",
    "-T",
    "linker/kernel.ld",
    "--defsym=__stack_top=0x810000",
  },
  link_inputs = {
    "linker/kernel.ld",
  },
  lang = {
    asm = {
      include_dirs = {
        "boot",
      },
      compile_options = {
        "-D__QSTAR_FREESTANDING_CORPUS__=1",
      },
      preprocess = true,
    },
  },
}

qstar.custom_target "kernel_img" {
  inputs = {
    qstar.target_file("//:kernel"),
  },
  outputs = {
    qstar.output("build/qstar/generated/kernel.img", {
      group = "images",
      format = "raw-binary",
      address = "0x80000",
      layout = "firmware-image",
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
    "//:kernel",
  },
}
