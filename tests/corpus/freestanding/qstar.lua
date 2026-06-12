qstar.project {
  name = "freestanding-corpus",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.profile "default" {
  toolchain = "clang",
  target = "aarch64-unknown-none-elf",
  arch = "aarch64",
  cpu = "cortex-a76",
  abi = "lp64",
  freestanding = true,
  cc = "tools/fake-clang.sh",
  linker = "tools/fake-link.sh",
  ar = "ar",
  sysroot = "sysroot",
  resource_dir = "resource",
  response_files = "on",
  response_style = "posix",
  link_options = {
    "-nostdlib",
  },
  tool_overrides = {
    "llvm-objcopy=tools/fake-objcopy.sh",
  },
}

qstar.config "kernel_c" {
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

qstar.executable "kernel" {
  configs = {
    "//:kernel_c",
  },
  sources = {
    "boot/start.S",
    "src/kernel.c",
  },
  linker_script = "linker/kernel.ld",
  defsyms = {
    "__stack_top=0x810000",
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
    "llvm-objcopy",
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
