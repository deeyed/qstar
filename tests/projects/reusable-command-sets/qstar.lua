qstar.project {
  name = "reusable-command-sets",
  version = "0.1.0",
  root = ".",
}

local commands = qstar.import_module("qstar/modules/commands")
qstar.command_set(commands)

qstar.command "direct" {
  description = qstar.status("Stable direct command remains available"),
  steps = {
    qstar.step.check("//..."),
  },
}
