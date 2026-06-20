qstar.project {
  name = "generic-command-artifact-workflow",
  version = "0.1.0",
  root = ".",
}

qstar.transform "payload_artifact" {
  input = "fixtures/payload.artifact",
  output = qstar.output("generated/artifacts/payload.artifact", {
    group = "artifacts",
  }),
  command = qstar.cli {
    "tools/transform-artifact.sh",
    qstar.input(0),
    qstar.output(0),
  },
  description = qstar.status("Transforming payload artifact"),
}

qstar.stage "workflow_layout" {
  root = "stage/workflow",
  description = qstar.status("Staging workflow layout"),
  files = {
    qstar.stage_file(qstar.target_file("//:payload_artifact"), "artifacts/payload.artifact"),
    qstar.stage_file("assets/manifest.txt", "manifest.txt"),
  },
}

qstar.stage "install_layout" {
  root = "stage/install",
  description = qstar.status("Staging install-style layout"),
  files = {
    qstar.stage_file(
      qstar.target_file("//:payload_artifact"),
      "share/generic-command-artifact-workflow/payload.artifact"
    ),
    qstar.stage_file(
      "assets/manifest.txt",
      "share/generic-command-artifact-workflow/manifest.txt"
    ),
  },
}

qstar.stage "package_layout" {
  root = "stage/package",
  description = qstar.status("Staging package layout"),
  files = {
    qstar.stage_file(qstar.target_file("//:payload_artifact"), "payload/payload.artifact"),
    qstar.stage_file("assets/manifest.txt", "metadata/manifest.txt"),
  },
}

qstar.run_target "artifact_smoke" {
  inputs = {
    qstar.target_file("//:payload_artifact"),
    qstar.stage_dir("//:workflow_layout"),
    "assets/manifest.txt",
  },
  command = qstar.cli {
    "tools/check-workflow.sh",
    "--artifact",
    qstar.input(0),
    "--layout",
    qstar.input(1),
    "--manifest",
    qstar.input(2),
    "--result",
    "generated/check/run-target.txt",
  },
  timeout = 5,
  expect = {
    contains = "WORKFLOW_OK",
    file = "generated/check/run-target.txt",
  },
  description = qstar.status("Checking workflow artifact inputs"),
}

qstar.command "workflow" {
  description = qstar.status("Build, check, and export the generic workflow layout"),
  aliases = {
    "workflow-export",
  },
  options = {
    out = qstar.param.path {
      default = "exports/workflow",
      description = "Export destination inside the package",
    },
    mode = qstar.param.enum {
      choices = {"fast", "full"},
      default = "fast",
    },
    verify = qstar.param.bool {
      default = true,
    },
  },
  steps = {
    qstar.step.build("//:payload_artifact"),
    qstar.step.stage("//:workflow_layout"),
    qstar.step.run {
      when = qstar.param("verify"),
      inputs = {
        qstar.target_file("//:payload_artifact"),
        qstar.stage_dir("//:workflow_layout"),
      },
      command = qstar.cli {
        "tools/check-workflow.sh",
        "--artifact",
        qstar.input(0),
        "--layout",
        qstar.input(1),
        "--mode",
        qstar.param("mode"),
        qstar.arg_if(qstar.param("verify"), "--verify"),
        "--result",
        "generated/check/project-command.txt",
      },
      timeout = 5,
      expect = {
        contains = "WORKFLOW_OK",
        file = "generated/check/project-command.txt",
      },
      description = qstar.status("Checking workflow command inputs"),
    },
    qstar.step.export_stage("//:workflow_layout", {
      to = qstar.param("out"),
    }),
  },
}

qstar.command "install" {
  description = qstar.status("Export the project-defined install layout"),
  aliases = {
    "install-local",
  },
  options = {
    out = qstar.param.path {
      default = "exports/install",
      description = "Install-style export destination inside the package",
    },
  },
  steps = {
    qstar.step.export_stage("//:install_layout", {
      to = qstar.param("out"),
    }),
  },
}

qstar.command "package-local" {
  description = qstar.status("Build and export a package-style layout"),
  options = {
    out = qstar.param.path {
      default = "exports/package",
      description = "Package-style export destination inside the package",
    },
  },
  steps = {
    qstar.step.build("//:payload_artifact"),
    qstar.step.stage("//:package_layout"),
    qstar.step.export_stage("//:package_layout", {
      to = qstar.param("out"),
    }),
  },
}

qstar.command "export-local" {
  description = qstar.status("Export the checked workflow layout"),
  options = {
    out = qstar.param.path {
      default = "exports/local",
      description = "Workflow export destination inside the package",
    },
  },
  steps = {
    qstar.step.export_stage("//:workflow_layout", {
      to = qstar.param("out"),
    }),
  },
}
