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

qstar.staticlib "variant" {
  configs = {"//:medium_c"},
  sources = {"src/variant.c"},
}

qstar.staticlib "product" {
  configs = {"//:medium_c"},
  sources = {"src/product.c"},
}

qstar.staticlib "module_start" {
  configs = {"//:medium_c"},
  sources = {"src/module_start.c"},
}

qstar.staticlib "module_cache" {
  configs = {"//:medium_c"},
  sources = {"src/module_cache.c"},
}

qstar.staticlib "module_runner" {
  configs = {"//:medium_c"},
  sources = {"src/module_runner.c"},
}

qstar.staticlib "plugin_stream" {
  configs = {"//:medium_c"},
  sources = {"src/plugin_stream.c"},
}

qstar.staticlib "plugin_timer" {
  configs = {"//:medium_c"},
  sources = {"src/plugin_timer.c"},
}

qstar.staticlib "plugin_storage" {
  configs = {"//:medium_c"},
  sources = {"src/plugin_storage.c"},
}

qstar.staticlib "runtime_clock" {
  configs = {"//:medium_c"},
  sources = {"src/runtime_clock.c"},
}

qstar.staticlib "runtime_power" {
  configs = {"//:medium_c"},
  sources = {"src/runtime_power.c"},
}

qstar.staticlib "service_network" {
  configs = {"//:medium_c"},
  sources = {"src/service_network.c"},
}

qstar.executable "medium_app" {
  configs = {"//:medium_c"},
  sources = {"src/main.c"},
  deps = {
    "//:core",
    "//:variant",
    "//:product",
    "//:module_start",
    "//:module_cache",
    "//:module_runner",
    "//:plugin_stream",
    "//:plugin_timer",
    "//:plugin_storage",
    "//:runtime_clock",
    "//:runtime_power",
    "//:service_network",
  },
}

qstar.group "all" {
  deps = {"//:medium_app"},
}
