qstar.project {
  name = "multipkg",
  version = "0.1.0",
  root = ".",
}

qstar.subdir("lib")
qstar.subdir("app")

qstar.stage "install_layout" {
  root = "build/qstar/stage/install",
  files = {
    qstar.stage_file(qstar.target_file("//lib:core"), "lib/libcore.a"),
    qstar.stage_file("lib/include/core.h", "include/core.h"),
  },
}

qstar.command "install" {
  options = {
    out = qstar.param.path {
      default = "exports/install",
    },
  },
  steps = {
    qstar.step.export_stage("//:install_layout", {
      to = qstar.param("out"),
    }),
  },
}
