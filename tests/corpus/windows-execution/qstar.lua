qstar.project {
  name = "windows-execution-corpus",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.toolset "msys2_ucrt64_gcc" {
  tools = {
    c = { compiler = qstar.cli {"gcc"} },
    cxx = { compiler = qstar.cli {"g++"} },
    asm = { compiler = qstar.cli {"gcc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"gcc"},
  },
  response_files = "on",
  response_style = "posix",
}

qstar.config "c_baseline" {
  toolset = "//:msys2_ucrt64_gcc",
  lang = {
    c = {
      public_include_dirs = {
        "include",
      },
      compile_options = {
        "-Wall",
        "-Wextra",
      },
    },
  },
}

qstar.config "long_response_args" {
  lang = {
    c = {
      compile_options = {
        "-DQSTAR_WINDOWS_EXECUTION_RSP_00=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_01=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_02=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_03=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_04=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_05=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_06=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_07=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_08=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_09=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_10=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_11=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_12=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_13=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_14=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_15=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_16=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_17=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_18=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_19=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_20=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_21=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_22=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_23=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_24=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_25=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_26=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_27=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_28=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_29=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_30=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_31=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_32=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_33=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_34=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_35=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_36=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_37=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_38=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_39=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_40=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_41=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_42=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_43=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_44=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_45=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_46=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_47=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_48=1",
        "-DQSTAR_WINDOWS_EXECUTION_RSP_49=1",
      },
    },
  },
}

qstar.executable "hello" {
  configs = {
    "//:c_baseline",
  },
  sources = {
    "src/hello.c",
  },
  artifact_name = "hello.exe",
}

qstar.staticlib "core" {
  configs = {
    "//:c_baseline",
  },
  sources = {
    "src/core.c",
  },
  artifact_name = "libwinexec_core.a",
}

qstar.executable "app" {
  configs = {
    "//:c_baseline",
  },
  sources = {
    "src/app.c",
  },
  deps = {
    "//:core",
  },
  artifact_name = "app.exe",
}

qstar.executable "response_probe" {
  configs = {
    "//:c_baseline",
    "//:long_response_args",
  },
  sources = {
    "src/response_probe.c",
  },
  artifact_name = "response_probe.exe",
}

qstar.custom_target "bridge_object" {
  inputs = {
    "src/bridge_payload.c",
  },
  outputs = {
    qstar.output("build/qstar/generated/bridge/bridge_payload.o", {
      format = "object",
    }),
  },
  command = qstar.cli {
    "tools/build-object.sh",
    qstar.input(0),
    qstar.output(0),
  },
  description = qstar.status("Building external object bridge_payload.o"),
}

qstar.executable "bridge_app" {
  configs = {
    "//:c_baseline",
  },
  sources = {
    "src/bridge_main.c",
    qstar.output("build/qstar/generated/bridge/bridge_payload.o"),
  },
  artifact_name = "bridge_app.exe",
}

qstar.group "all" {
  deps = {
    "//:hello",
    "//:app",
    "//:response_probe",
    "//:bridge_app",
  },
}
