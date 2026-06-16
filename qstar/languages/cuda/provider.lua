local P = {}

function P.tools(t)
  return qstar.provider_tools("cuda", {
    compiler = t.compiler,
  })
end

function P.options(t)
  return qstar.language_options("cuda", t or {})
end

function P.object(path, opts)
  return qstar.source(path, qstar.merge({
    language = "cuda",
    unit = "object",
  }, opts or {}))
end

function P.compile_object(ctx)
  local argv = qstar.argv()
  local arch = ctx.option("arch")

  argv:add(ctx.tool("compiler"))
  if arch ~= nil and arch ~= "" and arch ~= "native" then
    argv:add("-arch")
    argv:add(arch)
  end
  argv:add("-c")
  argv:add(ctx.input("source"))
  argv:add("-o")
  argv:add(ctx.output("object"))
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
