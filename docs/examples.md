# QStar Examples

> Current note: this file follows the v0.3/v0.4 authoring surface. The larger
> tutorial book lives under `qstar/wiki/`.

QStar examples use one entry file, `qstar.lua`, and optional subdirectory
fragments named `<folder>.qst`. Profile/toolchain settings are declared inside
QStar DSL with `qstar.profile`; QStar does not require a separate profile config
file.

## C Static Library And App

```txt
hello/
  qstar.lua
  lib/
    lib.qst
    include/core.h
    src/core.c
  app/
    app.qst
    src/main.c
```

Root file:

```lua
qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.profile "default" {
  toolchain = "clang",
  target = "host",
}

qstar.subdir("lib")
qstar.subdir("app")
```

Library fragment:

```lua
qstar.staticlib "core" {
  sources = {
    "lib/src/core.c",
  },
  lang = {
    c = {
      public_headers = {
        "lib/include/core.h",
      },
      public_include_dirs = {
        "lib/include",
      },
    },
  },
}
```

App fragment:

```lua
qstar.executable "app" {
  sources = {
    "app/src/main.c",
  },
  deps = {
    "//lib:core",
  },
}
```

Useful commands:

```txt
qstar check //...
qstar explain //app:app
qstar dry-run //app:app
qstar build //app:app
```

## Generated Config Header

Generated files are regular graph edges. `qstar.configure_file` is a built-in
action; `qstar.custom_target` is for external argv-vector commands.

```lua
qstar.configure_file "config" {
  output = qstar.output("generated/config.h"),
  values = {
    PROJECT_NAME = "demo",
    ENABLE_TRACE = "1",
  },
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/config.h"),
  },
  deps = {
    "//:config",
  },
  lang = {
    c = {
      include_dirs = {
        "generated",
      },
    },
  },
}
```

## Freestanding Firmware Shape

Firmware and OS-style builds are still generic targets. QStar does not need
dedicated UEFI or Raspberry Pi keywords; profile/link policy and custom
artifact transforms describe the graph.

```lua
qstar.profile "rpi5-aarch64" {
  toolchain = "clang",
  target = "aarch64-none-elf",
  arch = "aarch64",
  cpu = "cortex-a76",
  abi = "lp64",
  freestanding = true,
  cc = "clang",
  linker = "ld.lld",
  linker_script = "linker/rpi5-aarch64.ld",
  link_options = {
    "-nostdlib",
  },
  defsyms = {
    "__kernel_base=0x80000",
  },
  path_tools = {
    "llvm-objcopy",
    "qemu-system-aarch64",
  },
}

qstar.executable "kernel" {
  sources = {
    "boot/start.S",
    "src/kernel.c",
  },
  lang = {
    asm = {
      preprocess = true,
      compile_options = {
        "-ffreestanding",
      },
    },
    c = {
      compile_options = {
        "-ffreestanding",
      },
    },
  },
}

qstar.custom_target "kernel_img" {
  inputs = {
    qstar.target_file("//:kernel"),
  },
  outputs = {
    qstar.output("images/kernel8.img"),
  },
  command = qstar.cli {
    "llvm-objcopy",
    "-O",
    "binary",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.stage "rpi" {
  root = "stage/rpi",
  files = {
    qstar.stage_file("boot/config.txt", "config.txt"),
    qstar.stage_file(qstar.target_file("//:kernel_img"), "kernel8.img"),
  },
}
```

## Response Files

Response-file policy belongs to the active profile:

```lua
qstar.profile "windows-msvc" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "clang-cl",
  linker = "lld-link",
  response_files = "auto",
  response_style = "msvc",
}
```

Use `qstar dry-run` to inspect whether an action renders argv directly or uses
an `@response.rsp` file.
