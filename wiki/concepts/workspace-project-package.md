# Workspace, Project, Package

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다.
Workspace, project, package는 source tree를 graph로 해석하기 위한 경계다.

## 최소 예제

```lua
qstar.project {
  name = "demo",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  compile_commands = "build",
}
```

Root discovery는 현재 file에서 위로 올라가며 가장 가까운 `qstar.lua`를 찾는다. 별도
workspace marker file은 필요 없다.

`build_dir` 기본값은 `"build/qstar"`다. QStar의 state, log, response file,
object/artifact output, stage/install manifest, default `compile_commands.json`은 이
directory 아래에 모인다. Generated source/header/image output은 별도 정책인
`qstar.project.generated_dir` 아래에 있어야 한다. 기본값은 `"generated"`이고,
루트 오염을 피하고 싶으면 `"build/qstar/generated"`처럼 명시한다.
CLI `-B path`는 `qstar.project.build_dir`보다 우선한다. Override path도 package-relative
normalized path여야 하며 absolute path, `..`, `.`은 diagnostic으로 거부된다.

Generator는 CLI에서만 선택한다.

- `-G stella`: 현재 기본 Stella executor를 사용한다.
- `-G auto`: 현재는 `stella`로 resolve된다.
- `-G ninja`: C/C++/ASM compile, `qstar.configure_file`, `qstar.custom_target`,
  staticlib, sharedlib, executable/test link, `qstar.run_target`, `qstar.group` phony
  graph를 Ninja로 lower해 실행한다. `stage`와 `install`은 copy와 manifest를 QStar가
  처리하되, 참조 target artifact build에는 effective generator를 사용한다. Cale source
  action은 아직 Ninja로 lower되지 않으므로 `-G stella`가 필요하다. Windows shared
  library policy는 아직 deferred다.

Backend별 세부 지원 범위는 [Backends](../reference/backends.md)에 둔다.

`compile_commands`는 세 값을 가진다.

- `"build"`: 기본값. `build_dir/compile_commands.json`에 쓴다.
- `"root"`: clangd 호환이 필요할 때 project root의 `compile_commands.json`에 쓴다.
- `"off"`: compile database를 쓰지 않는다.

## 전체 예제

```txt
demo/
├── qstar.lua
├── app/
│   └── app.qst
└── lib/
    └── lib.qst
```

```lua
qstar.project {
  name = "demo",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  compile_commands = "build",
}

qstar.subdir("lib")
qstar.subdir("app")
```

`qstar.project.root`와 `QSTAR_PROJECT_ROOT`는 authoring helper에서 현재 project root를
참조할 때 사용한다.

## 실패 예제

```lua
qstar.project {
  name = "bad",
  root = "src",
}
```

v1에서 `root`는 `"."`만 허용된다.

## 관련 CLI

```sh
qstar --file qstar.lua query //app:app
qstar --file qstar.lua list-targets --format json
qstar --file qstar.lua -B out/qstar -G stella build //app:app
qstar --file qstar.lua emit-ninja //lib:core
qstar --file qstar.lua -G ninja build //lib:core
qstar --file app/app.qst build //app:app
```

## 관련 diagnostic

- `qstar.project root must be "." in v1`
- `qstar.project build_dir must be package-relative`
- `qstar.project compile_commands must be "root", "build", or "off"`
- `invalid generator`
- `CLI build directory override must be package-relative`
- `target label is owned by another package`
- `package root not found`
