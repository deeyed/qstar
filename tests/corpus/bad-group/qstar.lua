qstar.project {
  name = "bad-group-corpus",
  root = ".",
}

qstar.group "aggregate" {
  deps = {},
}

qstar.custom_target "uses_group_artifact" {
  inputs = {
    qstar.target_file("//:aggregate"),
  },
  outputs = {
    qstar.output("generated/aggregate.txt"),
  },
  command = qstar.cli {
    "tools/write-output.sh",
    qstar.output(0),
  },
}
