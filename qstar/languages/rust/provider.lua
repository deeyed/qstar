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

local function add_common_options(argv, ctx)
  for _, cfg in ipairs(ctx.option("cfg")) do
    argv:add("--cfg")
    argv:add(cfg)
  end
  for _, extern in ipairs(ctx.option("externs")) do
    argv:add("--extern")
    argv:add(extern)
  end
  argv:add_all(ctx.option("compile_options"))
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
  add_common_options(argv, ctx)

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

local function one_source(ctx)
  local sources = ctx.input("sources")
  if #sources ~= 1 then
    error("rust provider final actions require exactly one crate root source")
  end
  return sources, sources[1]
end

local function final_action(ctx, crate_type)
  local sources, source = one_source(ctx)
  local argv = qstar.argv()

  argv:add(ctx.tool("compiler"))
  argv:add("--edition")
  argv:add(ctx.option("edition"))
  argv:add("--crate-type")
  argv:add(crate_type)
  argv:add(source)
  argv:add("-o")
  argv:add(ctx.output("artifact"))
  add_common_options(argv, ctx)

  return {
    command = argv,
    inputs = sources,
    outputs = {
      ctx.output("artifact"),
    },
  }
end

function P.link_executable(ctx)
  return final_action(ctx, "bin")
end

function P.archive_staticlib(ctx)
  return final_action(ctx, "staticlib")
end

function P.link_sharedlib(ctx)
  return final_action(ctx, "cdylib")
end

return P
