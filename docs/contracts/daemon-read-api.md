# Stella Daemon Read API Contract

이 문서는 Stella IDE나 AI assistant가 QStar daemon을 내부 build service로 붙일 때 사용할
read-only API 계약이다. 이 API는 build/test/clean 같은 mutation을 열지 않는다. Mutating
작업은 기존 `qstar build`, `qstar test`, `qstar clean` command path와 사용자 승인 정책을
통해서만 실행한다.

## 상태

- Status: beta opt-in, read-only, not v1 stable
- Protocol magic: `qstar-daemon-query-v1`
- CLI helper: `qstar daemon --socket path --query method`
- Response: JSON text
- 기본 권한: read-only
- Remote access: out of scope
- Q249 regression: `make qstar-daemon-beta-boundary-tests` calls all current methods on socket-capable
  macOS/Linux hosts

## 보안 원칙

Daemon read API는 임의 파일 읽기 API가 아니다.

- client는 method 이름만 전달한다.
- method별로 추가 path argument를 받지 않는다.
- `compile_commands.path`와 `build.summary`는 현재 validated graph의 package root와
  effective build dir에서 계산한 path만 반환하거나 읽는다.
- source/header/build artifact path validation은 기존 QStar graph validation을 따른다.
- workspace root 밖 임의 path 탐색, glob, command execution은 read API에 없다.
- `build.request`, `test.request`, `clean.request` 같은 mutation RPC는 Q150 범위가 아니다.

## CLI

```sh
qstar --file qstar.lua -B build/qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query hello
qstar --file qstar.lua -B build/qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query workspace.info
qstar --file qstar.lua -B build/qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query targets.list
qstar --file qstar.lua -B build/qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query diagnostics.list
qstar --file qstar.lua -B build/qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query compile_commands.path
qstar --file qstar.lua -B build/qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query build.summary
```

`--file`, `-B`, `-G`, `--color`, `--progress`는 build command와 같은
effective graph identity를 만든다. IDE는 build panel, target tree, diagnostics panel이 모두
같은 identity를 보도록 같은 option set을 유지해야 한다.
다른 package root, entry file, build directory identity를 가진 daemon에 붙으면 read API도 build
client와 같은 방식으로 거부된다. 이 mismatch는 fallback 대상이 아니다.

## Methods

### `hello`

Daemon protocol availability와 method 목록을 반환한다. Graph를 load하지 않는다.

```json
{
  "schema": "qstar-daemon-read-v1",
  "method": "hello",
  "status": "ok",
  "experimental": true,
  "readonly": true,
  "methods": [
    "hello",
    "workspace.info",
    "targets.list",
    "diagnostics.list",
    "compile_commands.path",
    "build.summary"
  ]
}
```

### `workspace.info`

Validated graph의 project metadata와 effective build settings를 반환한다.

주요 field:

- `package_root`
- `project.name`
- `project.version`
- `project.build_dir`
- `project.generated_dir`
- `project.compile_commands`
- `project.generator`
- `target_count`
- `config_count`
- `generated_action_count`
- `stage_count`
- `diagnostic_count`

### `targets.list`

기존 `qstar list-targets --format json`과 같은 `qstar-targets-v1` JSON을 반환한다. IDE target
tree는 이 method를 기본 source로 쓴다.

### `diagnostics.list`

현재 daemon graph에 저장된 diagnostics를 반환한다. 이 method 자체는 새 deep lint pass를
실행하지 않는다. IDE의 실시간 error-check는 이후 별도 incremental diagnostics protocol로
강화할 수 있다.

```json
{
  "schema": "qstar-daemon-read-v1",
  "method": "diagnostics.list",
  "status": "ok",
  "readonly": true,
  "diagnostics": [],
  "summary": {
    "errors": 0,
    "warnings": 0,
    "infos": 0,
    "total": 0
  }
}
```

### `compile_commands.path`

`qstar.project.compile_commands` policy에 따른 compile database path를 반환한다. 파일을 새로
생성하지 않는다.

```json
{
  "schema": "qstar-daemon-read-v1",
  "method": "compile_commands.path",
  "status": "ok",
  "readonly": true,
  "policy": "build",
  "enabled": true,
  "path": "build/qstar/compile_commands.json",
  "absolute_path": "/workspace/project/build/qstar/compile_commands.json"
}
```

`compile_commands = "off"`이면 `enabled`는 `false`이고 `path`는 빈 문자열이다.

### `build.summary`

Effective build dir의 `state/last-summary.json`을 읽어 wrapper JSON으로 반환한다. 파일이
없어도 query는 성공이며 `exists=false`를 반환한다.

```json
{
  "schema": "qstar-daemon-read-v1",
  "method": "build.summary",
  "status": "ok",
  "readonly": true,
  "path": "build/qstar/state/last-summary.json",
  "absolute_path": "/workspace/project/build/qstar/state/last-summary.json",
  "exists": false
}
```

파일이 있으면 `summary` field에 `qstar-build-summary-v1` object가 들어간다.

## Stella IDE Naming

이 계약에서 `stella`는 QStar의 native executor/backend 이름이다. `Stella IDE`는 QStar를
내부 build service로 사용하는 별도 제품이다. Method 이름은 `workspace.info`,
`targets.list`처럼 generic build service namespace를 사용해서 IDE/AI API와 충돌하지 않게 한다.
