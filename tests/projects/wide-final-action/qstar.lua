local FINAL_OBJECTS <const> = qstar.option "final-objects" {
  type = "integer",
  value = 1000,
  description = "Number of prebuilt objects in the wide final action",
}

local DIRECT_SOURCES <const> = qstar.option "direct-sources" {
  type = "integer",
  value = 256,
  description = "Number of direct C sources compiled by the overflow fixture",
}

local wide1 = qstar.use_language("wide1")
local wide2 = qstar.use_language("wide2")

local function prebuilt_objects(count, offset)
  local result = {}
  local base = offset or 0

  for i = 0, count - 1 do
    result[#result + 1] = string.format(
      "objects/prebuilt/object-%04d.o",
      base + i
    )
  end
  return result
end

local function direct_sources(count)
  local result = {}

  for i = 0, count - 1 do
    result[#result + 1] = string.format("src/direct/unit-%04d.c", i)
  end
  return result
end

local function expected(count)
  return {
    string.format("--expect-objects=%d", count),
  }
end

local all_objects = prebuilt_objects(FINAL_OBJECTS)
local first_half_count = FINAL_OBJECTS // 2
local second_half_count = FINAL_OBJECTS - first_half_count
local generated_outputs = {}
local generated_command = {"tools/fake-generate-posix", "--generate-wide"}
local provider_v1_options = {}
local exact_48_command = {"tools/fake-command-posix"}
local exact_49_command = {"tools/fake-command-posix"}

for i = 0, 48 do
  local output = qstar.output(
    string.format("generated/objects/generated-%04d.o", i),
    {format = "object"}
  )
  generated_outputs[#generated_outputs + 1] = output
  generated_command[#generated_command + 1] = qstar.output(i)
end

for i = 0, 255 do
  provider_v1_options[#provider_v1_options + 1] =
    string.format("--provider-v1-option-%04d", i)
end

for i = 1, 46 do
  exact_48_command[#exact_48_command + 1] = "a"
end
exact_48_command[#exact_48_command + 1] = qstar.output(0)

for i = 1, 47 do
  exact_49_command[#exact_49_command + 1] = "a"
end
exact_49_command[#exact_49_command + 1] = qstar.output(0)

qstar.project {
  name = "wide-final-action",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "generated",
  compile_commands = "build",
  action_cache = "local",
}

qstar.toolset "response_on_tools" {
  tools = {
    c = {
      compiler = qstar.cli {"tools/fake-compile"},
    },
    archive = qstar.cli {"tools/fake-final-posix", "--tool-role=archive"},
    link = qstar.cli {"tools/fake-final-posix", "--tool-role=link"},
    wide1 = wide1.tools {
      compiler = qstar.cli {"tools/fake-final-posix", "--tool-role=wide1"},
    },
    wide2 = wide2.tools {
      compiler = qstar.cli {"tools/fake-final-posix", "--tool-role=wide2"},
    },
  },
  response_files = "on",
  response_style = "posix",
}

qstar.toolset "response_auto_tools" {
  tools = {
    c = {
      compiler = qstar.cli {"tools/fake-compile"},
    },
    archive = qstar.cli {"tools/fake-final-posix", "--tool-role=archive"},
    link = qstar.cli {"tools/fake-final-posix", "--tool-role=link"},
  },
  response_files = "auto",
  response_style = "posix",
}

qstar.toolset "response_off_tools" {
  tools = {
    c = {
      compiler = qstar.cli {"tools/fake-compile"},
    },
    archive = qstar.cli {"tools/fake-final-posix", "--tool-role=archive"},
    link = qstar.cli {"tools/fake-final-posix", "--tool-role=link"},
  },
  response_files = "off",
  response_style = "posix",
}

qstar.toolset "response_msvc_tools" {
  tools = {
    link = qstar.cli {"tools/fake-final-msvc", "--tool-role=link"},
  },
  response_files = "on",
  response_style = "msvc",
}

qstar.config "wide1_options" {
  toolset = "//:response_on_tools",
  lang = {
    wide1 = wide1.options {
      compile_options = provider_v1_options,
    },
  },
}

qstar.objectlib "wide_objects" {
  sources = all_objects,
}

qstar.objectlib "wide_objects_a" {
  sources = prebuilt_objects(first_half_count),
}

qstar.objectlib "wide_objects_b" {
  sources = prebuilt_objects(second_half_count, first_half_count),
}

for _, count in ipairs({0, 1, 48, 49, 252, 253, 256, 1000}) do
  local objectlib_name = string.format("cardinality_objects_%d", count)

  qstar.objectlib(objectlib_name) {
    sources = prebuilt_objects(count),
  }

  qstar.executable("cardinality_" .. count) {
    toolset = "//:response_on_tools",
    objects = {"//:" .. objectlib_name},
    link_options = expected(count),
  }
end

qstar.staticlib "wide_static" {
  toolset = "//:response_on_tools",
  objects = {"//:wide_objects"},
  link_options = expected(FINAL_OBJECTS),
}

qstar.sharedlib "wide_shared" {
  toolset = "//:response_on_tools",
  objects = {"//:wide_objects"},
  link_options = expected(FINAL_OBJECTS),
}

qstar.test "wide_test" {
  toolset = "//:response_on_tools",
  objects = {"//:cardinality_objects_256"},
  link_options = expected(256),
}

qstar.executable "direct_sources" {
  toolset = "//:response_on_tools",
  sources = direct_sources(DIRECT_SOURCES),
  link_options = expected(DIRECT_SOURCES),
}

qstar.objectlib "own_context" {
  toolset = "//:response_on_tools",
  compile_context = "own",
  sources = direct_sources(48),
}

qstar.executable "own_context_consumer" {
  toolset = "//:response_on_tools",
  objects = {"//:own_context"},
  link_options = expected(48),
}

qstar.objectlib "consumer_context" {
  compile_context = "consumer",
  sources = direct_sources(49),
}

qstar.executable "consumer_context_consumer" {
  toolset = "//:response_on_tools",
  objects = {"//:consumer_context"},
  link_options = expected(49),
}

qstar.executable "multiple_objectlibs" {
  toolset = "//:response_on_tools",
  objects = {
    "//:wide_objects_a",
    "//:wide_objects_b",
  },
  link_options = expected(FINAL_OBJECTS),
}

qstar.custom_target "generated_objects" {
  toolset = "//:response_on_tools",
  outputs = generated_outputs,
  command = qstar.cli(generated_command),
}

qstar.custom_target "logical_argc_48" {
  toolset = "//:response_on_tools",
  outputs = {qstar.output("generated/threshold/logical-48.stamp")},
  command = qstar.cli(exact_48_command),
}

qstar.custom_target "logical_argc_49" {
  toolset = "//:response_on_tools",
  outputs = {qstar.output("generated/threshold/logical-49.stamp")},
  command = qstar.cli(exact_49_command),
}

qstar.objectlib "generated_objectlib" {
  sources = generated_outputs,
}

qstar.executable "generated_bridge" {
  toolset = "//:response_on_tools",
  objects = {"//:generated_objectlib"},
  link_options = expected(49),
}

qstar.imported "imported_object" {
  artifact_kind = "prebuilt_object",
  artifacts = {
    default = {
      {
        id = "object",
        role = "link",
        path = "objects/imported/imported-object.o",
        primary = true,
      },
    },
  },
}

qstar.executable "imported_bridge" {
  toolset = "//:response_on_tools",
  deps = {"//:imported_object"},
  link_options = expected(1),
}

qstar.executable "response_auto" {
  toolset = "//:response_auto_tools",
  objects = {"//:cardinality_objects_253"},
  link_options = expected(253),
}

qstar.executable "response_off" {
  toolset = "//:response_off_tools",
  objects = {"//:cardinality_objects_256"},
  link_options = expected(256),
}

qstar.executable "response_msvc" {
  toolset = "//:response_msvc_tools",
  sources = {
    "objects/quoted/object with space.o",
  },
  link_options = {
    "--expect-objects=1",
    "option with space",
    "C:\\wide path\\tail\\",
    "quote\"inside",
    string.rep("m", 600),
  },
}

qstar.executable "provider_v1_wide" {
  configs = {"//:wide1_options"},
  sources = {"src/provider/main.w1"},
}

qstar.executable "provider_v2_mixed" {
  toolset = "//:response_on_tools",
  sources = {"src/provider/main.w2"},
  objects = {"//:wide_objects"},
}

qstar.group "fast_matrix" {
  deps = {
    "//:cardinality_0",
    "//:cardinality_1",
    "//:cardinality_48",
    "//:cardinality_49",
    "//:cardinality_252",
    "//:cardinality_253",
    "//:cardinality_256",
    "//:cardinality_1000",
    "//:wide_static",
    "//:wide_shared",
    "//:wide_test",
    "//:direct_sources",
    "//:own_context_consumer",
    "//:consumer_context_consumer",
    "//:multiple_objectlibs",
    "//:generated_bridge",
    "//:logical_argc_48",
    "//:logical_argc_49",
    "//:imported_bridge",
    "//:response_auto",
    "//:response_off",
    "//:response_msvc",
    "//:provider_v1_wide",
    "//:provider_v2_mixed",
  },
}
