local P = {}

function P.tools(t)
  return qstar.provider_tools("rust", {
    compiler = t.compiler,
  })
end

function P.options(t)
  return qstar.language_options("rust", t or {})
end

function P.object(path, opts)
  return qstar.source(path, qstar.merge({
    language = "rust",
    unit = "object",
  }, opts or {}))
end

function P.compile_object(ctx)
  local argv = qstar.argv()

  argv:add(ctx.tool("compiler"))
  argv:add("compile")
  argv:add("--source=" .. ctx.input("source"))
  argv:add("--object=" .. ctx.output("object"))
  argv:add("--depfile=" .. ctx.output("depfile"))
  argv:add_all(ctx.option("compile_options"))

  return {
    command = argv,
    inputs = {ctx.input("source")},
    outputs = {ctx.output("object")},
    depfile = ctx.output("depfile"),
  }
end

function P.archive_staticlib(ctx)
  local argv = qstar.argv()
  local inputs = {}

  argv:add(ctx.tool("compiler"))
  argv:add("archive")
  argv:add("--runtime=" .. ctx.output("archive"))
  argv:add("--metadata=" .. ctx.output("metadata"))
  argv:add("--depfile=" .. ctx.output("depfile"))
  for _, value in ipairs(ctx.input("sources")) do
    argv:add("source=" .. value)
    table.insert(inputs, value)
  end
  argv:add_all(ctx.option("compile_options"))

  return {
    command = argv,
    inputs = inputs,
    outputs = {
      ctx.output("archive"),
      ctx.output("metadata"),
    },
    depfile = ctx.output("depfile"),
  }
end

return P
