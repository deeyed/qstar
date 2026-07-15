qstar.project {
  name = "typed-dependency-negative-tool",
  version = "0.1.0",
  root = ".",
}

qstar.interface "not_a_tool" {}

qstar.custom_target "bad" {
  inputs = {"contracts/tool.in"},
  outputs = {qstar.output("generated/negative/output.txt")},
  command = qstar.cli {
    qstar.tool_file(":not_a_tool"),
    qstar.input(0),
    qstar.output(0),
  },
}
