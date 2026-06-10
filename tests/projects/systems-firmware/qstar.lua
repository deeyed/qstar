qstar.project {
  name = "systems-firmware",
  version = "0.1.0",
  root = ".",
}

qstar.executable "kernel" {
  sources = {
    "boot/start.S",
    "src/kernel.c",
  },
  linker_script = "linker/rpi5-aarch64.ld",
  defsyms = {
    "__stack_top=0x810000",
  },
  lang = {
    c = {
      compile_options = {
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
  outputs = {
    qstar.output("generated/kernel8.img", {
      group = "images",
      format = "raw-binary",
      address = "0x80000",
      layout = "rpi5-kernel8",
    }),
  },
  command = qstar.cli {
    "llvm-objcopy",
    "-O",
    "binary",
    qstar.target_file("//:kernel"),
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
  sources = {
    "src/efi_main.c",
  },
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
