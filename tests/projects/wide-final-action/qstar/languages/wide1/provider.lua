local P = {}

function P.tools(t)
  return qstar.provider_tools("wide1", {
    compiler = t.compiler,
  })
end

function P.options(t)
  return qstar.language_options("wide1", t or {})
end

function P.object(path, opts)
  return qstar.source(path, qstar.merge({
    language = "wide1",
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

  argv:add(ctx.tool("compiler"))
  argv:add("-o")
  argv:add(ctx.output("artifact"))
  argv:add_all(ctx.option("compile_options"))
  argv:add_all(sources)
  return {
    command = argv,
    inputs = sources,
    outputs = {ctx.output("artifact")},
  }
end

return P
