# Tutorial: Freestanding Image

QStar는 C/C++와 외부 object artifact bridge를 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Firmware나
bootloader식 flow는 special target이 아니라 profile, link option, custom target, stage
조합으로 표현한다.

## 최소 예제

```lua
qstar.executable "kernel" {
  sources = {
    "boot/start.S",
    "src/kernel.c",
  },
  link_options = {
    "-T",
    "linker/rpi5-aarch64.ld",
  },
  link_inputs = {
    "linker/rpi5-aarch64.ld",
  },
}
```

## 전체 예제

```lua
qstar.executable "kernel" {
  sources = {
    "boot/start.S",
    "src/kernel.c",
  },
  link_options = {
    "-T",
    "linker/rpi5-aarch64.ld",
    "--defsym=__rpi_load_addr=0x80000",
  },
  link_inputs = {
    "linker/rpi5-aarch64.ld",
  },
  lang = {
    c = {
      compile_options = {"-ffreestanding", "-fno-builtin"},
    },
    asm = {
      preprocess = true,
      compile_options = {"-ffreestanding"},
    },
  },
}

qstar.custom_target "kernel_img" {
  inputs = {qstar.target_file("//:kernel")},
  outputs = {
    qstar.output("generated/kernel8.img", {
      group = "images",
      format = "raw-binary",
      address = "0x80000",
      layout = "rpi5-kernel8",
    }),
  },
  command = qstar.cli {"llvm-objcopy", "-O", "binary", qstar.input(0), qstar.output(0)},
}
```

## 실패 예제

```lua
qstar.executable "kernel" {
  sources = {"boot/start.S"},
  link_inputs = {"../outside.ld"},
}
```

Link input도 package-relative path여야 한다.

## 관련 CLI

```sh
qstar --file qstar.lua --profile rpi5-aarch64 dry-run //:kernel
qstar --file qstar.lua build //:kernel_img
qstar --file qstar.lua stage //:rpi --dry-run
```

## 관련 diagnostic

- `linker script must be package-relative`
- `failure_kind=objcopy-failure`
- `output_identity=[...format=raw-binary...]`
