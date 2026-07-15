local P = {}

function P.tools(t)
  return qstar.provider_tools("legacy", {
    compiler = t.compiler,
  })
end

function P.object(path, opts)
  return qstar.source(path, qstar.merge({
    language = "legacy",
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

return P
