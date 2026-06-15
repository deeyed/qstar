qstar.project {
  name = "systems-firmware",
  version = "0.1.0",
  root = ".",
}

qstar.toolset "firmware_fake" {
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

qstar.toolset "uefi_fake" {
  tools = {
    c = qstar.cli {"tools/fake-clang.sh"},
    cxx = qstar.cli {"tools/fake-clang.sh"},
    asm = qstar.cli {"tools/fake-clang.sh"},
    archive = qstar.cli {"ar"},
    link = qstar.cli {"tools/fake-lld-link.sh"},
  },
  response_files = "on",
  response_style = "msvc",
}

qstar.config "firmware_tools" {
  toolset = "//:firmware_fake",
}

qstar.config "uefi_tools" {
  toolset = "//:uefi_fake",
}

qstar.executable "kernel" {
  configs = {
    "//:firmware_tools",
  },
  sources = {
    "boot/start.S",
    "src/kernel.c",
  },
  artifact_name = "kernel.elf",
  link_options = {
    "-nostdlib",
    "-Wl,-Map=kernel.map",
    "-T",
    "linker/rpi5-aarch64.ld",
    "--defsym=__stack_top=0x810000",
    "--defsym=__rpi_load_addr=0x80000",
  },
  link_inputs = {
    "linker/rpi5-aarch64.ld",
  },
  lang = {
    c = {
      compile_options = {
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-mgeneral-regs-only",
        "-mcpu=cortex-a76",
        "-mabi=lp64",
        "-fno-pic",
      },
    },
    asm = {
      include_dirs = {
        "boot",
      },
      compile_options = {
        "-D__QSTAR_FIRMWARE__=1",
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
    qstar.output("generated/kernel8.img", {
      group = "images",
      format = "raw-binary",
      address = "0x80000",
      layout = "rpi5-kernel8",
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

qstar.run_target "qemu_smoke" {
  deps = {
    "//:kernel",
  },
  command = qstar.cli {
    "tools/qemu-smoke.sh",
    qstar.target_file("//:kernel"),
    "serial.log",
  },
  timeout = 3,
  marker = "QSTAR-SMOKE-DONE",
  marker_log = "serial.log",
}

qstar.executable "uefi_boot" {
  configs = {
    "//:uefi_tools",
  },
  sources = {
    "src/efi_main.c",
  },
  artifact_name = "BOOTX64.EFI",
  link_options = {
    "/subsystem:efi_application",
    "/entry:efi_main",
    "/nodefaultlib",
  },
  lang = {
    c = {
      compile_options = {
        "-ffreestanding",
      },
    },
  },
}

qstar.executable "uefi_boot_aa64" {
  configs = {
    "//:uefi_tools",
  },
  sources = {
    "src/efi_main.c",
  },
  artifact_name = "BOOTAA64.EFI",
  link_options = {
    "/subsystem:efi_application",
    "/entry:efi_main",
    "/nodefaultlib",
  },
  lang = {
    c = {
      compile_options = {
        "-ffreestanding",
      },
    },
  },
}

qstar.stage "esp" {
  root = "stage/esp",
  files = {
    qstar.stage_file(qstar.target_file("//:uefi_boot"), "EFI/BOOT/BOOTX64.EFI"),
  },
}

qstar.stage "rpi" {
  root = "stage/rpi",
  files = {
    qstar.stage_file("boot/config.txt", "config.txt"),
    qstar.stage_file(qstar.target_file("//:kernel"), "kernel.elf"),
    qstar.stage_file(qstar.target_file("//:kernel_img"), "kernel8.img"),
    qstar.stage_file("boot/payload.bin", "payload.bin"),
  },
}
