qstar.project {
  name = "qstar",
  version = qstar.version,
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.profile "default" {
  response_files = "off",
}

qstar.import_file("qstar/policies/selfhost.qst")
local sources = qstar.import_module("qstar/modules/sources")

qstar.staticlib "lua_vendor" {
  configs = {
    "//qstar/policies:lua_vendor_c",
  },
  sources = sources.lua_vendor(),
}

qstar.staticlib "qstar_core" {
  configs = {
    "//qstar/policies:qstar_c",
  },
  sources = sources.qstar_core(),
  lang = {
    c = {
      public_headers = {
        "include/qstar/qstar.h",
      },
      public_include_dirs = {
        "include",
      },
      private_headers = {
        "src/internal.h",
      },
    },
  },
}

qstar.executable "qstar" {
  configs = {
    "//qstar/policies:qstar_c",
  },
  sources = {
    "src/main.c",
  },
  deps = {
    "//:qstar_core",
    "//:lua_vendor",
  },
  artifact_name = "qstar",
}

qstar.run_target "self_version" {
  deps = {
    "//:qstar",
  },
  command = qstar.cli {
    qstar.target_file("//:qstar"),
    "--version",
  },
  marker = "qstar ",
}

qstar.run_target "self_check_sample" {
  deps = {
    "//:qstar",
  },
  command = qstar.cli {
    qstar.target_file("//:qstar"),
    "--file",
    "tests/projects/c-app-lib-test/qstar.lua",
    "check",
  },
  marker = "status ok",
}

qstar.group "self_host" {
  deps = {
    "//:self_version",
    "//:self_check_sample",
  },
}
