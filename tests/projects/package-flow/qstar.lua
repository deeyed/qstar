qstar.project {
  name = "package-flow",
  version = "0.1.0",
  root = ".",
}

qstar.toolset "package_fake" {
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

qstar.toolset "msvc_fake" {
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

qstar.config "package_tools" {
  toolset = "//:package_fake",
}

qstar.config "msvc_tools" {
  toolset = "//:msvc_fake",
}

qstar.executable "module" {
  configs = {
    "//:package_tools",
  },
  sources = {
    "startup/start.S",
    "src/module.c",
  },
  artifact_name = "module.out",
  link_options = {
    "-Wl,-Map=module.map",
    "-T",
    "linker/package.ld",
    "--defsym=__module_base=0x1000",
  },
  link_inputs = {
    "linker/package.ld",
  },
  lang = {
    c = {
      compile_options = {
        "-std=c99",
        "-Wall",
        "-Wextra",
      },
    },
    asm = {
      include_dirs = {
        "startup",
      },
      compile_options = {
        "-D__QSTAR_PACKAGE_FLOW__=1",
      },
      preprocess = true,
    },
  },
}

qstar.custom_target "package_blob" {
  inputs = {
    qstar.target_file("//:module"),
  },
  outputs = {
    qstar.output("generated/package.bin", {
      group = "packages",
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

qstar.run_target "package_smoke" {
  deps = {
    "//:module",
  },
  command = qstar.cli {
    "tools/package-smoke.sh",
    qstar.target_file("//:module"),
    "smoke.log",
  },
  timeout = 3,
  expect = {
    contains = "QSTAR-SMOKE-DONE",
    file = "smoke.log",
  },
}

qstar.executable "msvc_payload" {
  configs = {
    "//:msvc_tools",
  },
  sources = {
    "src/payload_main.c",
  },
  artifact_name = "payload-x64.pe",
  link_options = {
    "/subsystem:console",
    "/entry:payload_main",
    "/nodefaultlib",
  },
  lang = {
    c = {
      compile_options = {
        "-std=c99",
      },
    },
  },
}

qstar.executable "msvc_payload_alt" {
  configs = {
    "//:msvc_tools",
  },
  sources = {
    "src/payload_main.c",
  },
  artifact_name = "payload-alt.pe",
  link_options = {
    "/subsystem:console",
    "/entry:payload_main",
    "/nodefaultlib",
  },
  lang = {
    c = {
      compile_options = {
        "-std=c99",
      },
    },
  },
}

qstar.stage "runtime" {
  root = "stage/runtime",
  files = {
    qstar.stage_file(qstar.target_file("//:msvc_payload"), "bin/payload-x64.pe"),
  },
}

qstar.stage "bundle" {
  root = "stage/bundle",
  files = {
    qstar.stage_file("assets/config.txt", "config.txt"),
    qstar.stage_file(qstar.target_file("//:module"), "module.out"),
    qstar.stage_file(qstar.target_file("//:package_blob"), "package.bin"),
    qstar.stage_file("assets/payload.bin", "payload.bin"),
  },
}
