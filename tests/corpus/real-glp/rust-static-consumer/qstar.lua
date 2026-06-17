local rust = qstar.use_language("rust")

qstar.project {
  name = "rust-static-consumer",
  version = "0.1",
  root = ".",
  build_dir = "build/qstar",
}

qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    rust = rust.tools {
      compiler = qstar.cli {"rustc"},
    },
  },
}

qstar.config "native" {
  toolset = "//:host",
  lang = {
    c = {
      compile_options = {"-std=c11", "-Wall", "-Wextra"},
    },
    rust = rust.options {
      edition = "2021",
      compile_options = {"--crate-type=lib"},
    },
  },
}

qstar.staticlib "rust_core" {
  configs = {"//:native"},
  sources = {
    "src/rust_core.rs",
  },
}

qstar.executable "consumer" {
  configs = {"//:native"},
  sources = {
    "src/consumer.c",
  },
  deps = {
    "//:rust_core",
  },
}

qstar.run_target "smoke" {
  deps = {
    "//:consumer",
  },
  command = qstar.cli {
    qstar.target_file("//:consumer"),
  },
  expect = {
    contains = "rust-value=77",
  },
}

