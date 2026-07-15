# Generic Command And Artifact Workflow

이 문서는 QStar의 generic workflow surface를 정리하고 drift guard 기준으로 봉인한다. 목적은 특정
언어, 특정 운영체제, 특정 toolchain, 특정 emulator, 특정 package format을 QStar 문법에
넣지 않고도 복잡한 project workflow를 표현하는 것이다.

QStar의 핵심 경계는 다음과 같다.

- QStar는 build graph, artifact dependency, copy layout, argv-vector execution,
  action-log/replay, dry-run/explain을 담당한다.
- 프로젝트는 source 의미론, domain-specific marker, emulator 정책, result schema,
  package 검사 규칙을 담당한다.
- 외부 command는 shell string이 아니라 항상 `qstar.cli { ... }` argv-vector다.
- 특정 언어 확장은 GLP provider 또는 object artifact bridge로 들어온다.

## 왜 이 기능들이 필요한가

현대적인 build graph에서는 다음 흐름이 흔하다.

```text
artifact 생성
-> artifact transform
-> copy-only layout stage
-> external smoke/check command
-> 사람이 치기 쉬운 project command
```

QStar는 `qstar.custom_target`, `qstar.transform`, `qstar.stage`, `qstar.run_target`,
`qstar.command`, typed command option, module import cache를 함께 제공한다.

이 문서는 그 표면을 generic하게 사용하는 정본 문법이다.

## 1. `qstar.run_target.inputs`

`qstar.run_target`은 external command를 실행하는 target이다. 새 `inputs` field는 run action이
실제로 소비하는 file/artifact/layout을 명시한다.

```lua
qstar.run_target "smoke" {
  inputs = {
    qstar.target_file("//:package_blob"),
    "fixtures/expected.txt",
  },
  command = qstar.cli {
    "tools/check-package.py",
    "--package", qstar.input(0),
    "--expected", qstar.input(1),
  },
  timeout = 10,
  expect = {
    contains = "CHECK_OK",
    file = "build/qstar/check/result.json",
  },
}
```

### Field

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `inputs` | list | no | Ordered run action input list. |
| `command` | `qstar.cli { ... }` | yes | Shell-free argv-vector command. |
| `timeout` | integer | no | Timeout in seconds. `0` means backend default/no timeout policy. |
| `expect` | table | no | Generic success text check. |
| `description` | `qstar.status("...")` | no | User-facing progress/action description. |
| `deps` | list label | no | Target dependency closure, not data input. |

### Input item kinds

| Item | Meaning |
| --- | --- |
| `"path/to/file"` | Package-relative plain file input. |
| `qstar.target_file("//:target")` | Primary artifact of a target or generated action. |
| `qstar.target_file("//:target", { artifact = "id" })` | Selected artifact of a multi-artifact target. |
| `qstar.stage_dir("//:layout")` | Staged layout root directory materialized before the consuming action. |

`qstar.input(N)` inside `run_target.command` resolves to the Nth `inputs` item. This mirrors
`custom_target.inputs` and keeps command argv independent from producer discovery.

### `deps`와 `inputs`의 차이

`deps`는 target dependency closure다. `inputs`는 action이 읽는 data/artifact 목록이다.
둘을 섞으면 build interface dependency와 runtime file dependency가 흐려진다. 따라서
generated artifact나 stage layout을 smoke command가 읽는 경우 canonical syntax는
`inputs`다.

## 2. Generated Artifact Consumption

Generic artifact transform은 단일 input/output이면 `qstar.transform`으로 표현한다.
복수 input/output이나 더 복잡한 generator는 underlying primitive인 `qstar.custom_target`을
사용한다.

```lua
qstar.toolset "artifact_tools" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  path_tools = {"package-object"},
}

qstar.transform "package_blob" {
  toolset = "//:artifact_tools",
  input = qstar.target_file("//:app"),
  output = qstar.output("build/qstar/generated/package/blob.bin"),
  command = qstar.cli {
    "package-object",
    qstar.input(0),
    qstar.output(0),
  },
}
```

Package-local executable을 PATH 이름이나 wrapper string으로 숨기지 않고 typed dependency로
연결하려면 `qstar.tool`과 `qstar.tool_file`을 쓴다.

```lua
qstar.tool "artifact_converter" {
  path = "tools/artifact-converter",
}

qstar.transform "converted" {
  input = qstar.target_file("//:app"),
  output = qstar.output("generated/converted.bin"),
  command = qstar.cli {
    qstar.tool_file("//:artifact_converter"),
    qstar.input(0),
    qstar.output(0),
  },
}
```

`qstar.tool_file`은 QStar-built executable/test도 받을 수 있으며 Stella/Ninja 양쪽에서
tool producer를 generated action보다 먼저 빌드한다. Typed dependency와 imported
artifact의 전체 계약은 `docs/typed-dependency-targets.md`에 있다.

핵심은 이 output을 모든 workflow surface가 같은 방식으로 소비하는 것이다.

Required behavior:

- `qstar.target_file("//:package_blob")`는 generated action output을 가리킨다.
- `run_target.inputs`에서 이 token을 쓰면 generated action이 먼저 실행된다.
- `qstar.stage_file(qstar.target_file("//:package_blob"), "...")`는 stage 전에 generated
  action을 실행한다.
- `qstar.command` step이 generated action label을 build하면 일반 target처럼 action-log와
  replay가 남는다.
- Stella와 Ninja는 같은 producer edge를 가져야 한다.

## 3. Stage As Consumable Layout

`qstar.stage`는 copy-only layout rule이다. Release bundle, SDK layout, test fixture,
device package, generated asset folder 같은 여러 상황에 사용할 수 있다.

```lua
qstar.stage "bundle" {
  root = "build/qstar/stage/bundle",
  files = {
    qstar.stage_file(qstar.target_file("//:package_blob"), "share/blob.bin"),
    qstar.stage_file("assets/config.json", "config.json"),
  },
}
```

Stage를 다른 action이 소비할 때는 `qstar.stage_dir(label)`을 사용한다.

```lua
qstar.run_target "bundle_check" {
  inputs = {
    qstar.stage_dir("//:bundle"),
  },
  command = qstar.cli {
    "tools/check-layout.py",
    "--root", qstar.input(0),
  },
}
```

Rules:

- `qstar.stage_dir(label)`는 typed token이다.
- 해당 stage는 consumer action 전에 실행된다.
- `qstar.input(N)`은 stage root path로 resolve된다.
- Stage manifest는 build metadata로 남는다.
- Stage는 package manager, installer, emulator, deploy tool이 아니다. 단순 layout
  materialization이다.

## 4. Project Commands: `qstar.command`

`qstar.command`는 root `qstar.lua`가 정의하는 user-facing project command다. Makefile의
phony target처럼 사람이 짧게 실행할 command를 제공하지만, shell script가 아니라 QStar
operation과 argv-vector action만 허용한다.

```lua
qstar.command "make-package" {
  description = qstar.status("Build package artifact"),
  steps = {
    qstar.step.build("//:package_blob"),
  },
}
```

Invocation:

```sh
qstar make-package
```

### 위치 제한

| Location | Policy |
| --- | --- |
| root `qstar.lua` | allowed |
| `.qst` graph fragment | forbidden |
| ordinary `.qsm` helper module | forbidden |
| language provider manifest/implementation | forbidden |

Project command는 public CLI surface이므로 root에서만 선언한다. Leaf fragment가 임의로
top-level command를 추가하면 project CLI가 fragment evaluation order에 묶인다.

### Fields

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `description` | `qstar.status("...")` | no | Command list/help/progress text. |
| `options` | table | no | Typed command option schema. |
| `env` | list/table | no | Environment entries applied to run steps; action-log/replay redacts values. |
| `working_dir` | string | no | Package-relative default working directory for run steps. |
| `steps` | list | yes | Ordered generic QStar steps. |
| `is_default` | bool | no | Marks the default project command; only one command may set it. |
| `hidden` | bool | no | Hides the command from listing while keeping it callable. |
| `aliases` | list string | no | Alternative command names. |

### Forbidden fields

| Field | Reason |
| --- | --- |
| `script` | Would reintroduce shell scripts as QStar DSL. |
| `shell` | Shell command strings break argv-vector determinism. |
| `sources` | Source ownership belongs to targets. |
| `outputs` | Output ownership belongs to targets, generated actions, or stage. |
| `deps` | Dependency closure belongs to target/run/stage inputs and command steps. |
| `language` | Project command must not know language semantics. |
| `platform` | Platform policy belongs to explicit command argv/config or target artifact policy. |

### Name rules

- Command name must be a plain CLI token.
- It must not start with `-`, `:`, `/`, or `@`.
- It must not contain path separators.
- It cannot collide with another command or alias.
- It cannot override reserved core commands.
- `install` is intentionally not reserved; projects may define it as a normal
  layout export command.

Reserved names initially include:

```text
build, test, stage, check, lint, fmt, clean, docs, daemon,
list-targets, query, doctor, explain, dry-run, emit-ninja, why-rebuild,
log, action-log, replay, last-failure, init
```

## 5. Command Options

Project commands can define typed options. Options are values, not code. They cannot trigger
shell expansion or arbitrary filesystem traversal.

```lua
qstar.command "package" {
  description = qstar.status("Create package layout"),
  is_default = false,
  options = {
    out = qstar.param.path {
      default = "build/qstar/stage/package",
    },
    mode = qstar.param.enum {
      choices = {"debug", "release"},
      default = "debug",
    },
    verbose = qstar.param.bool {
      default = false,
    },
  },
  steps = {
    qstar.step.stage("//:package_layout", {
      root = qstar.param("out"),
    }),
    qstar.step.run {
      when = qstar.param("verbose"),
      command = qstar.cli {
        "tools/inspect-package",
        qstar.param("out"),
        "--verbose",
      },
    },
  },
}
```

Option API:

| API | Value | Notes |
| --- | --- | --- |
| `qstar.param.string` | string | Plain scalar string. |
| `qstar.param.path` | path string | Package-relative by default unless the consuming step explicitly allows external destination. |
| `qstar.param.bool` | bool | CLI flag. |
| `qstar.param.int` | integer | Numeric value. |
| `qstar.param.enum` | string | One of declared `choices`. |
| `qstar.param.list` | list string | Repeated string value. |

Common option fields:

| Field | Applies to | Meaning |
| --- | --- | --- |
| `description` | all option types | User-facing option description. |
| `required` | non-bool option types | Missing value is an error. Cannot be combined with `default`. |
| `default` | all option types | Runtime value when the option is omitted. |
| `choices` | `qstar.param.enum` | Allowed enum values. |

Bool option CLI rules:

| Schema | CLI form | Runtime value |
| --- | --- | --- |
| `qstar.param.bool { default = false }` | option omitted | `false` |
| `qstar.param.bool { default = false }` | `--flag` or `--flag=true` | `true` |
| `qstar.param.bool { default = true }` | option omitted | `true` |
| `qstar.param.bool { default = true }` | `--no-flag` or `--flag=false` | `false` |

`required = true` is rejected for bool options in the initial design. A boolean flag should have a
stable default. If the user must make an explicit choice, use `qstar.param.enum` with explicit
`choices` instead.

`qstar.param("name")` references the runtime value inside command steps. For bool options, it can
also be used as a condition.

Bool helper surface:

| API | Scope | Meaning |
| --- | --- | --- |
| `when = qstar.param("flag")` | command step field | Skip the step unless the bool option is true. |
| `qstar.arg_if(cond, arg)` | inside `qstar.cli { ... }` | Append one argv atom only when `cond` is true. |
| `qstar.args_if(cond, args)` | inside `qstar.cli { ... }` | Append multiple argv atoms only when `cond` is true. |

Example:

```lua
qstar.command "probe" {
  options = {
    verbose = qstar.param.bool { default = false },
    trace = qstar.param.bool { default = true },
  },
  steps = {
    qstar.step.run {
      command = qstar.cli {
        "tools/probe",
        qstar.arg_if(qstar.param("verbose"), "--verbose"),
        qstar.args_if(qstar.param("trace"), {"--trace", "on"}),
      },
    },
  },
}
```

## 6. Command Steps

`qstar.command.steps` is an ordered list. Each step must be a generic QStar operation.

| Step API | Meaning |
| --- | --- |
| `qstar.step.build(label)` | Build a target, generated action, run target, or group label. |
| `qstar.step.test(label)` | Run test selection. |
| `qstar.step.stage(label, opts)` | Materialize stage layout. |
| `qstar.step.check(label)` | Validate graph/input closure. |
| `qstar.step.lint(label)` | Run authoring lint. |
| `qstar.step.run { ... }` | Execute a command-only argv-vector action. |
| `qstar.step.call(name, args)` | Invoke another project command. |
| `qstar.step.export_stage(label, opts)` | Materialize a stage and copy the layout to an explicit package-relative destination. |

### `qstar.step.run`

`qstar.step.run` is for command-local execution. Reusable smoke/check actions should still be
declared as `qstar.run_target`.

```lua
qstar.command "probe" {
  steps = {
    qstar.step.run {
      inputs = {
        qstar.target_file("//:tool"),
      },
      command = qstar.cli {
        qstar.input(0),
        "--version",
      },
      timeout = 5,
      expect = {
        contains = "tool",
      },
    },
  },
}
```

`qstar.step.run` fields:

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `inputs` | list | no | Same item kinds as `run_target.inputs`. |
| `command` | `qstar.cli { ... }` | yes | Shell-free argv-vector. |
| `timeout` | integer | no | Timeout in seconds. |
| `expect` | table | no | Same generic expectation surface as `run_target.expect`. |
| `description` | `qstar.status("...")` | no | Step progress/action description. |
| `env` | list/table | no | Redacted action environment. |
| `working_dir` | string | no | Package-relative working directory. |
| `when` | bool condition | no | Skip this step unless the condition is true. |

## 7. Install As Explicit Layout Export

QStar has no built-in `qstar install` artifact installer. Install, export, deploy,
flash, package, or publish workflows are project-owned commands built from generic
layout primitives. The generic model is:

```text
explicit layout -> optional external export command
```

Current syntax can express an explicit layout export as a project command:

```lua
qstar.stage "install_layout" {
  root = "build/qstar/stage/install",
  files = {
    qstar.stage_file(qstar.target_file("//:tool"), "bin/tool"),
    qstar.stage_file("README.md", "share/doc/tool/README.md"),
  },
}

qstar.command "install" {
  aliases = {"install-local"},
  options = {
    out = qstar.param.path { default = "exports/install" },
  },
  steps = {
    qstar.step.export_stage("//:install_layout", {
      to = qstar.param("out"),
    }),
  },
}
```

This avoids hard-coding Unix-only assumptions into every project. A CLI tool can
define an `install` command. A packaged-output project can define a `bundle`
command. A board bring-up project can define a `flash` command. QStar sees only
layout and copy/export semantics.

The canonical regression fixture keeps three user-facing command names around
this model:

```sh
qstar install --out exports/install
qstar install-local --out exports/install-local
qstar package-local --out exports/package
qstar export-local --out exports/local
```

All four are root `qstar.command` entries in
`tests/projects/generic-command-artifact-workflow/qstar.lua`; none are built-in
artifact installers.

## 8. `qstar.import_module` Cache/Reuse

`qstar.import_module("folder/path")` loads helper module exports from
`folder/path/path.qsm`. Helper modules are not graph fragments. They should be reusable.

Target behavior:

```lua
local paths = qstar.import_module("qstar/modules/paths")
local paths_again = qstar.import_module("qstar/modules/paths")
```

Rules:

- Same canonical module path evaluates once.
- Repeated imports return cached exports.
- Circular module import remains an error.
- `qstar.import_file` remains graph-fragment evaluation and is not cached like a helper module.
- `.qsm` modules still cannot declare `qstar.project`, `qstar.toolset`, `qstar.config`,
  targets, stages, commands, `qstar.import_file`, or `qstar.subdir`.
- Cached exports are read-only.

If user code mutates a module export, QStar produces a diagnostic that says module exports are
read-only.

## 9. `qstar.transform`

`qstar.transform` is sugar over `qstar.custom_target`. It makes single-input/single-output artifact
transforms more readable while keeping the same generic semantics.

```lua
qstar.toolset "artifact_tools" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  path_tools = {"pack"},
}

qstar.transform "packed" {
  toolset = "//:artifact_tools",
  input = qstar.target_file("//:app"),
  output = qstar.output("build/qstar/generated/packed.bin"),
  command = qstar.cli {
    "pack",
    qstar.input(0),
    qstar.output(0),
  },
}
```

Rules:

- It lowers to the same generated action contract as `qstar.custom_target`.
- It has exactly one `input` and one `output`.
- It accepts the same `toolset` and `description` policy fields as `qstar.custom_target`.
- If `toolset` is set, a bare PATH command tool must be listed in that toolset's `path_tools`.
- It must not know any particular file format.
- It must not imply compile, link, archive, packaged-output, runner, package, or language semantics.
- Docs should teach `custom_target` as the underlying primitive and `transform` as single-artifact
  readability sugar.

## 10. Backend And Diagnostic Contract

Every feature in this document must work through both Stella and Ninja.

Required parity:

- dry-run shows the same logical producer/consumer closure.
- explain shows run inputs, command steps, stage layout inputs, and generated action producers.
- action-log records resolved argv, redacted env, inputs, outputs, timeout, expect metadata.
- replay reconstructs the same command context.
- failures preserve existing classes such as exit-code, timeout, expect-missing, package-failure,
  malformed-input, unknown-label, and duplicate-command.
- generated/stage producer edges do not disappear in Ninja phony lowering.

Diagnostics should mention the declaring file and line whenever possible.

## 11. Generic Boundary Checklist

새 문법을 public surface에 추가하기 전에 다음 질문을 모두 통과해야 한다.

| Question | Required answer |
| --- | --- |
| Does it name a language, compiler, OS, board, emulator, image format, or package manager? | No |
| Can it describe CLI tools, libraries, packaged bundles, generated assets, SDK layouts, and smoke tests? | Yes |
| Is shell-string execution still absent? | Yes |
| Are artifacts owned by targets/generated actions/stages, not commands? | Yes |
| Can Stella and Ninja share the same action contract? | Yes |
| Can `explain`, `dry-run`, `action-log`, and `replay` work without domain knowledge? | Yes |
| Can downstream projects keep marker/policy/result logic outside QStar? | Yes |

## 12. Seal Status

이 workflow surface는 다음 gate로 봉인한다.

1. `tests/smoke.sh`는 문법, diagnostic, action-log/replay, docs/wiki/man/snippet drift를 확인한다.
2. `tests/projects/generic-command-artifact-workflow`는 transform, stage input, run input,
   project command option, bool argv helper, explicit layout export, project-defined
   `install`/`install-local`/`package-local`/`export-local` command를 하나의 fixture로 묶는다.
3. `tests/ninja-backend-parity.sh`는 같은 fixture를 Stella와 Ninja 양쪽에서 실행해 producer
   edge가 backend별로 갈라지지 않는지 확인한다.
4. `make check`와 GitHub Actions Linux Validation은 이 seal을 기본 회귀 경로에 포함한다.
