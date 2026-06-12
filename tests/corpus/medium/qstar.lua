qstar.project {
  name = "medium-corpus",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  compile_commands = "build",
}

qstar.config "medium_c" {
  lang = {
    c = {
      compile_options = {
        "-std=c99",
        "-Wall",
        "-Wextra",
      },
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:medium_c"},
  sources = {"src/core.c"},
}

qstar.staticlib "arch" {
  configs = {"//:medium_c"},
  sources = {"src/arch.c"},
}

qstar.staticlib "board" {
  configs = {"//:medium_c"},
  sources = {"src/board.c"},
}

qstar.staticlib "kernel_boot" {
  configs = {"//:medium_c"},
  sources = {"src/kernel_boot.c"},
}

qstar.staticlib "kernel_mm" {
  configs = {"//:medium_c"},
  sources = {"src/kernel_mm.c"},
}

qstar.staticlib "kernel_sched" {
  configs = {"//:medium_c"},
  sources = {"src/kernel_sched.c"},
}

qstar.staticlib "driver_serial" {
  configs = {"//:medium_c"},
  sources = {"src/driver_serial.c"},
}

qstar.staticlib "driver_timer" {
  configs = {"//:medium_c"},
  sources = {"src/driver_timer.c"},
}

qstar.staticlib "driver_storage" {
  configs = {"//:medium_c"},
  sources = {"src/driver_storage.c"},
}

qstar.staticlib "platform_clock" {
  configs = {"//:medium_c"},
  sources = {"src/platform_clock.c"},
}

qstar.staticlib "platform_power" {
  configs = {"//:medium_c"},
  sources = {"src/platform_power.c"},
}

qstar.staticlib "network_stack" {
  configs = {"//:medium_c"},
  sources = {"src/network_stack.c"},
}

qstar.executable "medium_app" {
  configs = {"//:medium_c"},
  sources = {"src/main.c"},
  deps = {
    "//:core",
    "//:arch",
    "//:board",
    "//:kernel_boot",
    "//:kernel_mm",
    "//:kernel_sched",
    "//:driver_serial",
    "//:driver_timer",
    "//:driver_storage",
    "//:platform_clock",
    "//:platform_power",
    "//:network_stack",
  },
}

qstar.group "all" {
  deps = {"//:medium_app"},
}
