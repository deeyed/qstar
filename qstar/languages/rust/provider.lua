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
  argv:add("--edition")
  argv:add(ctx.option("edition"))
  argv:add("--crate-type")
  argv:add(ctx.option("crate_type"))
  argv:add("--emit=obj")
  argv:add(ctx.input("source"))
  argv:add("-o")
  argv:add(ctx.output("object"))
  for _, cfg in ipairs(ctx.option("cfg")) do
    argv:add("--cfg")
    argv:add(cfg)
  end
  for _, extern in ipairs(ctx.option("externs")) do
    argv:add("--extern")
    argv:add(extern)
  end
  argv:add_all(ctx.option("compile_options"))

  return {
    command = argv,
    inputs = {
      ctx.input("source"),
    },
    outputs = {
      ctx.output("object"),
    },
  }
end

return P
