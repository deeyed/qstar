# Generic Workflows

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 이 문서는
언어, 운영체제, compiler, file format, package manager를 QStar builtin으로 추가하지 않고도
project-local workflow를 표현하는 표준 문법을 정리한다.

핵심 원칙은 단순하다. Artifact 생산은 target이나 generated action이 소유하고, layout은
`qstar.stage`가 소유하며, 사람이 치는 짧은 명령은 root `qstar.command`가 여러 generic step을
순서대로 호출한다. 외부 도구 실행은 항상 `qstar.cli { ... }` argv-vector로 작성한다.

## 최소 예제

```lua
qstar.toolset "artifact_tools" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  path_tools = {"transform-artifact"},
}

qstar.transform "payload_artifact" {
  toolset = "//:artifact_tools",
  input = "fixtures/payload.artifact",
  output = qstar.output("generated/artifacts/payload.artifact"),
  command = qstar.cli {
    "transform-artifact",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.run_target "smoke" {
  inputs = {
    qstar.target_file("//:payload_artifact"),
  },
  command = qstar.cli {
    "tools/check-artifact.sh",
    qstar.input(0),
  },
}
```

`qstar.transform`은 단일 input을 단일 output으로 바꾸는 `qstar.custom_target` sugar다.
별도 산출물 종류를 만들지 않고 같은 generated action contract로 낮아진다. 복수
input/output이 필요하면 `qstar.custom_target`을 사용한다. Generated action이 bare PATH
tool을 실행한다면 `toolset`을 지정하고 그 toolset의 `path_tools`에 tool 이름을 넣는다.
이 규칙은 code generation, object conversion, package-local artifact transform, external
validation 등 모든 generic workflow에 같은 방식으로 적용된다.

## 전체 예제

```lua
qstar.transform "payload_artifact" {
  toolset = "//:artifact_tools",
  input = "fixtures/payload.artifact",
  output = qstar.output("generated/artifacts/payload.artifact", {
    group = "artifacts",
  }),
  command = qstar.cli {
    "transform-artifact",
    qstar.input(0),
    qstar.output(0),
  },
  description = qstar.status("Transforming payload artifact"),
}

qstar.stage "workflow_layout" {
  root = "stage/workflow",
  description = qstar.status("Staging workflow layout"),
  files = {
    qstar.stage_file(qstar.target_file("//:payload_artifact"),
      "artifacts/payload.artifact"),
    qstar.stage_file("assets/manifest.txt", "manifest.txt"),
  },
}

qstar.run_target "artifact_smoke" {
  inputs = {
    qstar.target_file("//:payload_artifact"),
    qstar.stage_dir("//:workflow_layout"),
    "assets/manifest.txt",
  },
  command = qstar.cli {
    "tools/check-workflow.sh",
    "--artifact", qstar.input(0),
    "--layout", qstar.input(1),
    "--manifest", qstar.input(2),
  },
  timeout = 5,
  expect = {
    contains = "WORKFLOW_OK",
    file = "generated/check/run-target.txt",
  },
  description = qstar.status("Checking workflow artifact inputs"),
}

qstar.command "workflow" {
  description = qstar.status("Build, check, and export the workflow layout"),
  aliases = {"workflow-export"},
  options = {
    out = qstar.param.path {
      default = "exports/workflow",
    },
    mode = qstar.param.enum {
      choices = {"fast", "full"},
      default = "fast",
    },
    verify = qstar.param.bool {
      default = true,
    },
  },
  steps = {
    qstar.step.build("//:payload_artifact"),
    qstar.step.stage("//:workflow_layout"),
    qstar.step.run {
      when = qstar.param("verify"),
      inputs = {
        qstar.target_file("//:payload_artifact"),
        qstar.stage_dir("//:workflow_layout"),
      },
      command = qstar.cli {
        "tools/check-workflow.sh",
        "--artifact", qstar.input(0),
        "--layout", qstar.input(1),
        "--mode", qstar.param("mode"),
        qstar.arg_if(qstar.param("verify"), "--verify"),
      },
      expect = {
        contains = "WORKFLOW_OK",
        file = "generated/check/project-command.txt",
      },
    },
    qstar.step.export_stage("//:workflow_layout", {
      to = qstar.param("out"),
    }),
  },
}
```

이 예제에서 `qstar.target_file("//:payload_artifact")`는 generated artifact를 first-class
producer로 추적한다. `qstar.stage_dir("//:workflow_layout")`는 stage root를 action input으로
선언하고, command argv에는 `qstar.input(N)`으로 넘긴다. 따라서 Stella와 Ninja 모두
consumer action 전에 transform과 stage producer를 먼저 실행한다.

`qstar.step.export_stage`는 explicit layout export다. QStar에는 built-in `qstar install`
artifact installer가 없으며, project-specific layout export는 command 안에서 명시적으로
작성한다. `qstar.command "install"`은 허용된다. 이 이름도 `deploy`, `flash`, `publish`,
`bundle`과 마찬가지로 프로젝트가 정의하는 user-facing command일 뿐이다.

Canonical fixture는 같은 원칙을 `install`, `install-local`, `package-local`,
`export-local` 네 command로 봉인한다. 모두 root `qstar.command`이며 최종 copy/export는
`qstar.step.export_stage`가 담당한다.

## 실패 예제

```lua
qstar.run_target "bad" {
  command = qstar.cli {
    "tools/check-workflow.sh",
    qstar.stage_dir("//:workflow_layout"),
  },
}
```

`qstar.stage_dir(...)`는 command argv에 직접 넣는 값이 아니라 `inputs`에 선언한 뒤
`qstar.input(N)`으로 참조해야 한다. 이렇게 해야 producer edge와 argv path가 같은 선언에서
관리된다.

## 관련 CLI

```sh
qstar --file qstar.lua explain //:artifact_smoke
qstar --file qstar.lua build //:artifact_smoke
qstar --file qstar.lua -G ninja build //:artifact_smoke
qstar --file qstar.lua commands
qstar --file qstar.lua workflow --out exports/local --mode full
qstar --file qstar.lua install --out exports/install
qstar --file qstar.lua install-local --out exports/install-local
qstar --file qstar.lua package-local --out exports/package
qstar --file qstar.lua export-local --out exports/local
qstar --file qstar.lua action-log qstar-command:workflow:run:2
qstar --file qstar.lua replay qstar-command:workflow:run:2
```

The repository fixture
`tests/projects/generic-command-artifact-workflow` exercises the same flow with
Stella and Ninja. It is intentionally named after generic artifact and layout
concepts so public syntax does not grow domain-specific target kinds.

## 관련 diagnostic

- `qstar.stage_dir is only valid in run_target inputs`
- `qstar.stage_dir in run_target command must also be declared in inputs`
- `project command name 'build' is reserved`
- `unknown transform field`
- `transform '//:name' requires input`
- `generated action '//:name' references unknown toolset`
- `generated action PATH tool '...' is not allowed by toolset path_tools`
