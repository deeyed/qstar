qstar.project {
  name = "response-files-corpus",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.toolset "host_rsp" {
  tools = {
    c = qstar.cli {"cc"},
    cxx = qstar.cli {"c++"},
    asm = qstar.cli {"cc"},
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  response_files = "on",
  response_style = "posix",
  path_tools = {"qstar-argv-probe"},
}

qstar.toolset "windows_fake" {
  tools = {
    c = qstar.cli {"tools/fake-clang-cl"},
    cxx = qstar.cli {"tools/fake-clang-cl"},
    asm = qstar.cli {"tools/fake-clang-cl"},
    archive = qstar.cli {"tools/fake-lib"},
    link = qstar.cli {"tools/fake-clang-cl"},
  },
  response_files = "on",
  response_style = "msvc",
  path_tools = {"qstar-argv-probe"},
}

qstar.config "host_rsp_tools" {
  toolset = "//:host_rsp",
}

qstar.config "windows_fake_tools" {
  toolset = "//:windows_fake",
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
  configs = {"//:host_rsp_tools", "//:long_c_command"},
  sources = {
    "src/main.c",
  },
}

qstar.custom_target "argv_probe" {
  outputs = {
    qstar.output("build/qstar/generated/argv.txt"),
  },
  command = qstar.cli {
    "tools/argv-probe.sh",
    qstar.output(0),
    "alpha beta",
    "quote ' value",
    "semi;colon",
    "dollar$value",
  },
}

qstar.executable "windows_app" {
  configs = {"//:windows_fake_tools", "//:long_c_command"},
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
    "//:windows_fake_tools",
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
  configs = {"//:windows_fake_tools", "//:long_c_command"},
  sources = {
    "src/main.c",
  },
  artifact_name = "mapped_named.exe",
}

qstar.staticlib "windows_static" {
  configs = {"//:windows_fake_tools", "//:long_c_command"},
  sources = {
    "src/main.c",
  },
  artifact_name = "windows_static.lib",
}

qstar.sharedlib "windows_plugin" {
  configs = {"//:windows_fake_tools"},
  sources = {
    "src/main.c",
  },
}

qstar.group "all" {
  deps = {
    "//:app",
  },
}
