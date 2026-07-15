local P = {}

function P.tools(t)
  return qstar.provider_tools("pack", {
    compiler = t.compiler,
  })
end

function P.options(t)
  return qstar.language_options("pack", t or {})
end

function P.object(path, opts)
  return qstar.source(path, qstar.merge({
    language = "pack",
    unit = "object",
  }, opts or {}))
end

function P.compile_object(ctx)
  local argv = qstar.argv()

  argv:add(ctx.tool("compiler"))
  argv:add("compile")
  argv:add(ctx.input("source"))
  argv:add(ctx.output("object"))
  return {
    command = argv,
    inputs = {ctx.input("source")},
    outputs = {ctx.output("object")},
  }
end

function P.link_executable(ctx)
  local argv = qstar.argv()
  local inputs = {}

  argv:add(ctx.tool("compiler"))
  argv:add("final")
  argv:add(ctx.output("runtime"))
  argv:add(ctx.output("metadata"))
  argv:add(ctx.output("resources"))
  argv:add(ctx.output("link"))

  for _, value in ipairs(ctx.input("sources")) do
    argv:add("source=" .. value)
    table.insert(inputs, value)
  end
  for _, value in ipairs(ctx.input("objects")) do
    argv:add("object=" .. value)
    table.insert(inputs, value)
  end
  for _, value in ipairs(ctx.input("link_interfaces")) do
    argv:add("interface=" .. value)
    table.insert(inputs, value)
  end
  for _, value in ipairs(ctx.input("link_inputs")) do
    argv:add("link-input=" .. value)
    table.insert(inputs, value)
  end
  for _, value in ipairs(ctx.input("link_options")) do
    argv:add("link-option=" .. value)
  end

  return {
    command = argv,
    inputs = inputs,
    outputs = {
      ctx.output("runtime"),
      ctx.output("metadata"),
      ctx.output("resources"),
      ctx.output("link"),
    },
  }
end

return P
