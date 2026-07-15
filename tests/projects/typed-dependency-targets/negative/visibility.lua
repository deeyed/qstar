qstar.project {
  name = "typed-dependency-negative-visibility",
  version = "0.1.0",
  root = ".",
}

qstar.tool "private_tool" {
  path = "tools/copy-tool.sh",
  visibility = {"//negative/allowed:..."},
}

qstar.import_file("negative/blocked/blocked.qst")
