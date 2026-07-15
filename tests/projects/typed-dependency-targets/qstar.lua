qstar.project {
  name = "typed-dependency-targets",
  version = "0.1.0",
  root = ".",
}

local host_link_usage = {}
if qstar.host.os ~= "windows" then
  host_link_usage = {"-pthread"}
end

qstar.tool "copy_tool" {
  path = "tools/copy-tool.sh",
}

qstar.imported "imported_copy_tool" {
  artifact_kind = "host_tool",
  artifacts = {
    default = {
      {
        id = "executable",
        role = "tool",
        path = "tools/copy-tool.sh",
        primary = true,
      },
    },
  },
}

qstar.custom_target "generated_contract" {
  inputs = {"contracts/generated.in"},
  outputs = {qstar.output("generated/contracts/generated.txt")},
  command = qstar.cli {
    qstar.tool_file("//:copy_tool"),
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.interface "base_usage" {
  compile_usage = {
    options = {"-DQSTAR_BASE_USAGE=1"},
    inputs = {qstar.target_file("//:generated_contract")},
  },
}

qstar.imported "private_usage" {
  artifact_kind = "private_staticlib",
  artifacts = {
    default = {
      {
        id = "archive",
        role = "link",
        path = "vendor/libprivate.a",
        primary = true,
      },
    },
  },
  compile_usage = {
    options = {"-DQSTAR_PRIVATE_USAGE=1"},
  },
}

qstar.interface "api_usage" {
  deps = {"//:base_usage"},
  private_deps = {"//:private_usage"},
  compile_usage = {
    options = {"-DQSTAR_API_USAGE=1"},
  },
}

qstar.imported "vendor" {
  artifact_kind = "staticlib",
  artifacts = {
    default = {
      {
        id = "runtime",
        role = "runtime",
        path = "vendor/default.runtime",
        primary = true,
      },
      {
        id = "archive",
        role = "link",
        path = "vendor/libvendor-default.a",
        primary = false,
      },
    },
    darwin = {
      {
        id = "runtime",
        role = "runtime",
        path = "vendor/darwin.runtime",
        primary = true,
      },
      {
        id = "archive",
        role = "link",
        path = "vendor/libvendor-darwin.a",
        primary = false,
      },
    },
    linux = {
      {
        id = "runtime",
        role = "runtime",
        path = "vendor/linux.runtime",
        primary = true,
      },
      {
        id = "archive",
        role = "link",
        path = "vendor/libvendor-linux.a",
        primary = false,
      },
    },
    windows = {
      {
        id = "runtime",
        role = "runtime",
        path = "vendor/windows.runtime",
        primary = true,
      },
      {
        id = "archive",
        role = "link",
        path = "vendor/libvendor-windows.a",
        primary = false,
      },
    },
  },
  deps = {"//:api_usage"},
  compile_usage = {
    options = {"-DQSTAR_VENDOR_USAGE=1"},
  },
  link_usage = {
    options = host_link_usage,
    inputs = {"contracts/link.contract"},
  },
}

qstar.staticlib "middle" {
  sources = {"src/middle.c"},
  deps = {"//:vendor"},
}

qstar.sharedlib "plugin" {
  sources = {"src/plugin.c"},
  deps = {"//:vendor"},
}

qstar.executable "app" {
  sources = {"src/main.c"},
  deps = {"//:middle"},
}

qstar.executable "built_tool" {
  sources = {"tools/built-tool.c"},
}

qstar.custom_target "built_tool_output" {
  inputs = {"contracts/tool.in"},
  outputs = {qstar.output("generated/tool/output.txt")},
  command = qstar.cli {
    qstar.tool_file("//:built_tool"),
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.custom_target "runtime_copy" {
  inputs = {qstar.target_file("//:vendor")},
  outputs = {qstar.output("generated/runtime/selected.txt")},
  command = qstar.cli {
    qstar.tool_file("//:copy_tool"),
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.custom_target "imported_tool_output" {
  inputs = {"contracts/tool.in"},
  outputs = {qstar.output("generated/tool/imported.txt")},
  command = qstar.cli {
    qstar.tool_file("//:imported_copy_tool"),
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.group "all" {
  deps = {
    "//:app",
    "//:plugin",
    "//:built_tool_output",
    "//:runtime_copy",
    "//:imported_tool_output",
  },
}
