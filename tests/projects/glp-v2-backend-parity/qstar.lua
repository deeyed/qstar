local zig = qstar.use_language("zig")

local rust = qstar.use_language("rust")

local cuda = qstar.use_language("cuda")

local parity_args = {}

for i = 1, 56 do
  table.insert(parity_args, string.format("--glp-v2-parity-%02d", i))
end

qstar.project {
  name = "glp-v2-backend-parity",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  compile_commands = "build",
}

qstar.toolset "host" {
  tools = {
    c = {
      compiler = qstar.cli {"cc"},
    },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    zig = zig.tools {
      compiler = qstar.cli {"tools/fake-provider.sh", "zig"},
    },
    rust = rust.tools {
      compiler = qstar.cli {"tools/fake-provider.sh", "rust"},
    },
    cuda = cuda.tools {
      compiler = qstar.cli {"tools/fake-provider.sh", "cuda"},
    },
  },
  response_files = "on",
  response_style = "posix",
}

qstar.config "parity" {
  toolset = "//:host",
  lang = {
    zig = zig.options {
      env_value = "alpha",
      compile_options = parity_args,
    },
    rust = rust.options {
      compile_options = {"--rust-v2-parity"},
    },
    cuda = cuda.options {
      compile_options = {"--cuda-v2-parity"},
    },
  },
}

qstar.staticlib "rust_archive" {
  configs = {
    "//:parity",
  },
  sources = {
    "src/rustlib.rs",
  },
}

qstar.sharedlib "cuda_plugin" {
  configs = {
    "//:parity",
  },
  sources = {
    "src/cuda_plugin.cu",
  },
}

qstar.executable "mixed" {
  configs = {
    "//:parity",
  },
  sources = {
    "src/main.zig",
    "src/support.rs",
    "src/kernel.cu",
    "src/native.c",
  },
  deps = {
    "//:rust_archive",
    "//:cuda_plugin",
  },
  link_inputs = {
    "vendor/explicit.link",
  },
  link_options = {
    "--explicit-v2-link-option",
  },
}

qstar.executable "consumer" {
  configs = {
    "//:parity",
  },
  sources = {
    "src/consumer.zig",
  },
  deps = {
    "//:mixed",
  },
}

qstar.group "all" {
  deps = {
    "//:rust_archive",
    "//:cuda_plugin",
    "//:mixed",
    "//:consumer",
  },
}
