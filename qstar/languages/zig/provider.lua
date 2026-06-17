local P = {}

function P.tools(t)
  return qstar.provider_tools("zig", {
    compiler = t.compiler,
  })
end

function P.options(t)
  return qstar.language_options("zig", t or {})
end

function P.object(path, opts)
  return qstar.source(path, qstar.merge({
    language = "zig",
    unit = "object",
  }, opts or {}))
end

function P.compile_object(ctx)
  local argv = qstar.argv()
  local target = ctx.option("target")

  argv:add(ctx.tool("compiler"))
  argv:add("build-obj")
  argv:add(ctx.input("source"))
  argv:add("-O")
  argv:add(ctx.option("optimize"))
  if target ~= nil and target ~= "" and target ~= "native" then
    argv:add("-target")
    argv:add(target)
  end
  argv:add("-femit-bin=" .. ctx.output("object"))
  argv:add_all(ctx.option("compile_options"))

  return {
    command = argv,
    env = {
      "ZIG_GLOBAL_CACHE_DIR=" .. ctx.cache("zig-global"),
      "ZIG_LOCAL_CACHE_DIR=" .. ctx.cache("zig-local"),
    },
    inputs = {
      ctx.input("source"),
    },
    outputs = {
      ctx.output("object"),
    },
  }
end

return P
