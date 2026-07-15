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

local function action_env(ctx)
  return {
    "QSTAR_GLP_V2_ENV=" .. ctx.option("env_value"),
    "QSTAR_GLP_V2_CACHE=" .. ctx.cache("zig-v2"),
  }
end

local function add_options(argv, ctx)
  argv:add_all(ctx.option("compile_options"))
end

function P.compile_object(ctx)
  local argv = qstar.argv()

  argv:add(ctx.tool("compiler"))
  argv:add("compile")
  argv:add("--source=" .. ctx.input("source"))
  argv:add("--object=" .. ctx.output("object"))
  argv:add("--depfile=" .. ctx.output("depfile"))
  add_options(argv, ctx)

  return {
    command = argv,
    env = action_env(ctx),
    inputs = {ctx.input("source")},
    outputs = {ctx.output("object")},
    depfile = ctx.output("depfile"),
  }
end

function P.link_executable(ctx)
  local argv = qstar.argv()
  local inputs = {}

  argv:add(ctx.tool("compiler"))
  argv:add("executable")
  argv:add("--runtime=" .. ctx.output("runtime"))
  argv:add("--metadata=" .. ctx.output("metadata"))
  argv:add("--resources=" .. ctx.output("resources"))
  argv:add("--import=" .. ctx.output("import"))
  argv:add("--depfile=" .. ctx.output("depfile"))

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
  add_options(argv, ctx)

  return {
    command = argv,
    env = action_env(ctx),
    inputs = inputs,
    outputs = {
      ctx.output("runtime"),
      ctx.output("metadata"),
      ctx.output("resources"),
      ctx.output("import"),
    },
    depfile = ctx.output("depfile"),
  }
end

return P
