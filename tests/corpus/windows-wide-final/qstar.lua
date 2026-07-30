local OBJECT_COUNT <const> = qstar.option "object-count" {
  type = "integer",
  value = 1000,
}

local objects = {}

for i = 0, OBJECT_COUNT - 1 do
  objects[#objects + 1] = string.format("objects/object-%04d.obj", i)
end

qstar.project {
  name = "windows-wide-final",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "generated",
}

qstar.toolset "msvc_response" {
  tools = {
    link = qstar.cli {"tools/fake-link.exe"},
  },
  response_files = "on",
  response_style = "msvc",
}

qstar.objectlib "objects" {
  sources = objects,
}

qstar.executable "app" {
  toolset = "//:msvc_response",
  objects = {"//:objects"},
}
