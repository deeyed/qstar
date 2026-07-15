local malformed_case = qstar.option "malformed_case" {
  type = "combo",
  value = "ok",
  choices = {
    "ok",
    "project_unknown",
    "toolset_type",
    "toolset_unknown",
    "config_unknown",
    "artifact_type",
    "objectlib_forbidden",
    "group_unknown",
    "stage_type",
    "stage_unknown",
    "target_family_type",
    "target_family_unknown",
    "custom_target_unknown",
    "transform_type",
    "configure_unknown",
    "run_target_type",
    "command_type",
    "command_unknown",
    "command_step_mutated",
    "command_option_unknown",
    "option_type",
    "option_unknown",
    "variant_unknown",
    "list_shape",
  },
}

if malformed_case == "project_unknown" then
  qstar.project { name = "malformed", typo = true } -- CASE:project_unknown
else
  qstar.project {
    name = "malformed-declarations",
    version = "1",
    root = ".",
  }
end

if malformed_case == "toolset_type" then
  qstar.toolset "bad_tools" { -- CASE:toolset_type
    tools = {archive = qstar.cli {"ar"}},
    path_tools = "objcopy",
  }
elseif malformed_case == "toolset_unknown" then
  qstar.toolset "bad_tools" {tools = {}, path_tool = {"objcopy"}} -- CASE:toolset_unknown
elseif malformed_case == "config_unknown" then
  qstar.config "bad_config" {typo = true} -- CASE:config_unknown
elseif malformed_case == "artifact_type" then
  qstar.executable "bad_app" {sources = "main.c"} -- CASE:artifact_type
elseif malformed_case == "objectlib_forbidden" then
  qstar.objectlib "bad_objects" {link_options = {"-s"}} -- CASE:objectlib_forbidden
elseif malformed_case == "group_unknown" then
  qstar.group "bad_group" {sources = {"main.c"}} -- CASE:group_unknown
elseif malformed_case == "stage_type" then
  qstar.stage "bad_stage" {files = "layout"} -- CASE:stage_type
elseif malformed_case == "stage_unknown" then
  qstar.stage "bad_stage" {file = {}} -- CASE:stage_unknown
elseif malformed_case == "target_family_type" then
  qstar.target_family "bad_family" { -- CASE:target_family_type
    allow_shared_sources = "yes",
    targets = {"//:app"},
  }
elseif malformed_case == "target_family_unknown" then
  qstar.target_family "bad_family" {target = {"//:app"}} -- CASE:target_family_unknown
elseif malformed_case == "custom_target_unknown" then
  qstar.custom_target "bad_generated" { -- CASE:custom_target_unknown
    outputs = {qstar.output("generated/out.txt")},
    command = qstar.cli {"tools/write", qstar.output(0)},
    typo = true,
  }
elseif malformed_case == "transform_type" then
  qstar.transform "bad_transform" { -- CASE:transform_type
    input = "qstar.lua",
    output = false,
    command = qstar.cli {"tools/copy", qstar.input(0), qstar.output(0)},
  }
elseif malformed_case == "configure_unknown" then
  qstar.configure_file "bad_configure" { -- CASE:configure_unknown
    output = "generated/config.h",
    typo = true,
  }
elseif malformed_case == "run_target_type" then
  qstar.run_target "bad_run" { -- CASE:run_target_type
    command = qstar.cli {"true"},
    timeout = "fast",
  }
elseif malformed_case == "command_type" then
  qstar.command "bad_command" {hidden = "yes"} -- CASE:command_type
elseif malformed_case == "command_unknown" then
  qstar.command "bad_command" {step = {}} -- CASE:command_unknown
elseif malformed_case == "command_step_mutated" then
  local step = qstar.step.build("//...")
  step.typo = true
  qstar.command "bad_command" {steps = {step}} -- CASE:command_step_mutated
elseif malformed_case == "command_option_unknown" then
  local option = qstar.param.string {typo = true} -- CASE:command_option_unknown
  qstar.command "bad_command" {options = {value = option}}
elseif malformed_case == "option_type" then
  qstar.option "bad_option" {type = true, value = "x"} -- CASE:option_type
elseif malformed_case == "option_unknown" then
  qstar.option "bad_option" {type = "string", default = "x"} -- CASE:option_unknown
elseif malformed_case == "variant_unknown" then
  qstar.variant "bad_variant" {values = {arch = "sample"}, typo = true} -- CASE:variant_unknown
elseif malformed_case == "list_shape" then
  qstar.executable "bad_list" {sources = {main = "main.c"}} -- CASE:list_shape
else
  local helper = qstar.import_module("qstar/modules/free-table")
  assert(helper.typo == "ordinary Lua metadata")
  local sample = qstar.variant "sample" {
    values = {
      arch = "sample-arch",
      triple = "sample-none",
      user = {mode = "custom", levels = {1, 2, 3}},
    },
    tags = {"positive"},
  }
  assert(sample.values.user.mode == "custom")
  qstar.group "ok" {}
end
