qstar.project {
  name = "bad-import-corpus",
  root = ".",
}

local common = qstar.import_module("modules/common/common.qsm")

qstar.group "all" {
  deps = common.deps,
}
