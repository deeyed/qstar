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

local function effective_target(target, macos_min_version)
  if target == nil or target == "" then
    return nil
  end
  if macos_min_version ~= nil and macos_min_version ~= "" then
    if target == "native" and qstar.host.os == "macos" then
      return qstar.host.arch .. "-macos." .. macos_min_version
    end
    if string.match(target, "%-macos$") then
      return target .. "." .. macos_min_version
    end
  end
  return target
end

local function add_common_options(argv, ctx)
  local target = effective_target(
    ctx.option("target"),
    ctx.option("macos_min_version")
  )

  argv:add("-O")
  argv:add(ctx.option("optimize"))
  if target ~= nil and target ~= "" and target ~= "native" then
    argv:add("-target")
    argv:add(target)
  end
  argv:add_all(ctx.option("compile_options"))
end

local function cache_env(ctx)
  return {
    "ZIG_GLOBAL_CACHE_DIR=" .. ctx.cache("zig-global"),
    "ZIG_LOCAL_CACHE_DIR=" .. ctx.cache("zig-local"),
  }
end

function P.compile_object(ctx)
  local argv = qstar.argv()

  argv:add(ctx.tool("compiler"))
  argv:add("build-obj")
  argv:add(ctx.input("source"))
  add_common_options(argv, ctx)
  argv:add("-femit-bin=" .. ctx.output("object"))

  return {
    command = argv,
    env = cache_env(ctx),
    inputs = {
      ctx.input("source"),
    },
    outputs = {
      ctx.output("object"),
    },
  }
end

local function final_action(ctx, subcommand, mode_flag)
  local argv = qstar.argv()
  local sources = ctx.input("sources")

  argv:add(ctx.tool("compiler"))
  argv:add(subcommand)
  argv:add_all(sources)
  if mode_flag ~= nil then
    argv:add(mode_flag)
  end
  add_common_options(argv, ctx)
  argv:add("-femit-bin=" .. ctx.output("artifact"))

  return {
    command = argv,
    env = cache_env(ctx),
    inputs = sources,
    outputs = {
      ctx.output("artifact"),
    },
  }
end

function P.link_executable(ctx)
  return final_action(ctx, "build-exe", nil)
end

function P.archive_staticlib(ctx)
  return final_action(ctx, "build-lib", "-static")
end

function P.link_sharedlib(ctx)
  return final_action(ctx, "build-lib", "-dynamic")
end

return P
