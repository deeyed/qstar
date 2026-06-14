qstar.project {
  name = "response-files-corpus",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.profile "default" {
  response_files = "on",
  response_style = "posix",
  tool_overrides = {
    "qstar-argv-probe=tools/argv-probe.sh",
  },
}

qstar.profile "windows-msvc" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "clang-cl",
  cxx = "clang-cl",
  linker = "clang-cl",
  response_files = "on",
  response_style = "msvc",
  tool_overrides = {
    "qstar-argv-probe=tools/argv-probe.sh",
  },
}

qstar.profile "windows-msvc-fake" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "tools/fake-clang-cl",
  cxx = "tools/fake-clang-cl",
  linker = "tools/fake-clang-cl",
  response_files = "on",
  response_style = "msvc",
  tool_overrides = {
    "qstar-argv-probe=tools/argv-probe.sh",
  },
}

qstar.profile "windows-msvc-artifact-map" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "clang-cl",
  cxx = "clang-cl",
  linker = "clang-cl",
  response_files = "on",
  response_style = "msvc",
  tool_overrides = {
    "qstar-argv-probe=tools/argv-probe.sh",
  },
  artifact_names = {
    "//:windows_mapped=profile_named.exe",
  },
}

qstar.profile "windows-msvc-static-artifact-map" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "clang-cl",
  cxx = "clang-cl",
  linker = "clang-cl",
  response_files = "on",
  response_style = "msvc",
  tool_overrides = {
    "qstar-argv-probe=tools/argv-probe.sh",
  },
  artifact_names = {
    "//:windows_static=windows_static.lib",
  },
}

qstar.profile "windows-msvc-static-fake" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "tools/fake-clang-cl",
  cxx = "tools/fake-clang-cl",
  ar = "tools/fake-lib",
  linker = "tools/fake-clang-cl",
  response_files = "on",
  response_style = "msvc",
  tool_overrides = {
    "qstar-argv-probe=tools/argv-probe.sh",
  },
  artifact_names = {
    "//:windows_static=windows_static.lib",
  },
}

qstar.config "long_c_command" {
  lang = {
    c = {
      include_dirs = {
        "include/very/long/path/segment/000",
        "include/very/long/path/segment/001",
        "include/very/long/path/segment/002",
        "include/very/long/path/segment/003",
        "include/very/long/path/segment/004",
        "include/very/long/path/segment/005",
        "include/very/long/path/segment/006",
        "include/very/long/path/segment/007",
        "include/very/long/path/segment/008",
        "include/very/long/path/segment/009",
        "include/very/long/path/segment/010",
        "include/very/long/path/segment/011",
        "include/very/long/path/segment/012",
        "include/very/long/path/segment/013",
        "include/very/long/path/segment/014",
        "include/very/long/path/segment/015",
        "include/very/long/path/segment/016",
        "include/very/long/path/segment/017",
        "include/very/long/path/segment/018",
        "include/very/long/path/segment/019",
      },
      compile_options = {
        "-Wall",
        "-Wextra",
      },
    },
  },
}

qstar.config "msvc_response_escape_args" {
  lang = {
    c = {
      compile_options = {
        "/DNAME=alpha beta",
        "/DQUOTE=\"value\"",
        "/DTRAIL=tail\\",
        "/DSEMICOLON=a;b",
        "/DWINPATH=C:\\Program Files\\QStar\\Include",
        "/DJSON={\"path\":\"C:\\qstar\\include\"}",
        "/DSPACE_TRAIL=value with trailing space ",
        "/DSLASHQUOTE=C:\\qstar\\\"quoted\"",
      },
    },
  },
}

qstar.executable "app" {
  configs = {"//:long_c_command"},
  sources = {
    "src/main.c",
  },
}

qstar.custom_target "argv_probe" {
  outputs = {
    qstar.output("build/qstar/generated/argv.txt"),
  },
  command = qstar.cli {
    "qstar-argv-probe",
    qstar.output(0),
    "alpha beta",
    "quote ' value",
    "semi;colon",
    "dollar$value",
  },
}

qstar.executable "windows_app" {
  configs = {"//:long_c_command"},
  sources = {
    "src/main.c",
  },
  artifact_name = "windows_app.exe",
  lib_dirs = {
    "sdk/lib/um/x64",
  },
  libs = {
    "kernel32",
  },
}

qstar.executable "windows_rsp" {
  configs = {
    "//:long_c_command",
    "//:msvc_response_escape_args",
  },
  sources = {
    "src/main.c",
  },
  artifact_name = "windows_rsp.exe",
  lib_dirs = {
    "sdk/lib with space/um/x64",
  },
  libs = {
    "kernel32",
    "user32",
    "advapi32",
    "shell32",
    "ole32",
    "uuid",
  },
  link_options = {
    "/DEBUG:FULL",
    "/PDB:build/qstar/pdb/windows rsp.pdb",
    "/MANIFESTDEPENDENCY:type='win32' name='QStar Probe'",
    "/DEFAULTLIB:msvcrt",
    "/DEFAULTLIB:vcruntime",
    "/DEFAULTLIB:ucrt",
    "/INCREMENTAL:NO",
    "/OPT:REF",
    "/OPT:ICF",
    "/SUBSYSTEM:CONSOLE",
    "/ENTRY:mainCRTStartup",
    "/NODEFAULTLIB:oldnames",
    "/SAFESEH:NO",
    "/DYNAMICBASE",
    "/NXCOMPAT",
    "/LARGEADDRESSAWARE",
    "/BREPRO",
    "/GUARD:CF",
    "/MACHINE:X64",
    "/VERSION:0.5",
  },
}

qstar.executable "windows_mapped" {
  configs = {"//:long_c_command"},
  sources = {
    "src/main.c",
  },
}

qstar.staticlib "windows_static" {
  configs = {"//:long_c_command"},
  sources = {
    "src/main.c",
  },
}

qstar.sharedlib "windows_plugin" {
  sources = {
    "src/main.c",
  },
}

qstar.group "all" {
  deps = {
    "//:app",
  },
}
