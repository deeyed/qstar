local P = {}

function P.tools(t)
  return qstar.provider_tools("wide2", {
    compiler = t.compiler,
  })
end

function P.options(t)
  return qstar.language_options("wide2", t or {})
end

function P.object(path, opts)
  return qstar.source(path, qstar.merge({
    language = "wide2",
    unit = "object",
  }, opts or {}))
end

function P.compile_object(ctx)
  local argv = qstar.argv()

  argv:add(ctx.tool("compiler"))
  argv:add("-c")
  argv:add(ctx.input("source"))
  argv:add("-o")
  argv:add(ctx.output("object"))
  return {
    command = argv,
    inputs = {ctx.input("source")},
    outputs = {ctx.output("object")},
  }
end

function P.link_executable(ctx)
  local argv = qstar.argv()
  local sources = ctx.input("sources")
  local objects = ctx.input("objects")
  local inputs = {}

  argv:add(ctx.tool("compiler"))
  argv:add("-o")
  argv:add(ctx.output("runtime"))
  argv:add("--expect-objects=" .. #objects)
  argv:add_all(sources)
  argv:add_all(objects)
  for _, value in ipairs(sources) do
    inputs[#inputs + 1] = value
  end
  for _, value in ipairs(objects) do
    inputs[#inputs + 1] = value
  end
  return {
    command = argv,
    inputs = inputs,
    outputs = {ctx.output("runtime")},
  }
end

return P
