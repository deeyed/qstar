# CMake-Style Progress Output Contract

이 문서는 Round Q101에서 고정하는 QStar build progress 출력 계약이다. 구현은 다음
라운드에서 순차 적용한다. 현재 문서의 목적은 Stella executor와 Ninja backend가 같은
사용자 경험을 향하도록 user-facing format을 먼저 확정하는 것이다.

```txt
status: progress output contract
target line: qstar 0.5
default backend: stella
reference style: CMake-style action progress
implementation status: Stella progress renderer and warning/error stream coloring active
```

## Default Format

일반 build 출력은 action description 중심으로 표시한다.

```txt
[  8%] Building C object build/qstar/out/__core/obj0.o
[ 23%] Building CXX object build/qstar/out/__ui/obj4.o
warning: src/ui/view.cpp:42: unused variable 'tmp'
[ 51%] Linking C static library libcore.a
[ 75%] Linking CXX executable app
[100%] Built target app
```

Rules:

- Percent는 대괄호 안에서 3칸 폭으로 오른쪽 정렬한다.
- 예: `[  5%]`, `[ 75%]`, `[100%]`.
- Percent 뒤에는 legacy scheduler stage wording이나 state name이 아니라 build action description이 온다.
- 일반 output에서 prepare/stage progress line, `Status: compiling ...`, `schedule_action`,
  `build_action id=...` 같은 내부 trace는 출력하지 않는다.
- `qstar.group` 같은 dependency-only target은 progress action으로 세지 않는다.
- No-op/cache-hit action은 compact하게 처리하며, 필요할 때만 `--verbose`에서 설명한다.

## Built-In Descriptions

QStar가 자동 생성하는 기본 description은 다음 wording을 사용한다.

| Action kind | Description |
| --- | --- |
| C compile | `Building C object <object>` |
| C++ compile | `Building CXX object <object>` |
| ASM compile | `Building ASM object <object>` |
| C static library | `Linking C static library <artifact>` |
| C++ static library | `Linking CXX static library <artifact>` |
| C executable | `Linking C executable <artifact>` |
| C++ executable | `Linking CXX executable <artifact>` |
| configure file | `Configuring <output>` |
| custom target | `Generating <output>` |
| run target | `Running <label>` |
| stage | `Staging <label>` |
| install | `Installing <artifact>` |
| final target | `Built target <name>` |

Stella와 Ninja backend는 가능한 한 같은 description을 사용한다. Ninja backend는
QStar action description을 Ninja `description = ...`로 lower한다.

## Action Description IR

Round Q102부터 Stella action plan은 사용자-facing description을 별도 field로 가진다.
`qstar explain`, `qstar dry-run`, `--verbose`, `--schedule-trace`에서는 다음 line으로 확인할
수 있다.

```txt
action_description id=//:app:compile:0 text="Building C object build/qstar/out/___app/obj0.o"
action_description id=//:app:link:0 text="Linking C executable build/qstar/out/___app/app"
```

`qstar.group`과 `true` aggregate 같은 no-op run target은 progress action으로 세지 않는다.
Dry-run/explain은 group exclusion을 다음처럼 표시한다.

```txt
progress_action label=//:all include=no reason=group
```

## Authoring Hook

사용자 정의 action은 `qstar.status(...)`로 progress description을 지정한다.

```lua
qstar.custom_target "version_header" {
  outputs = {
    qstar.output("build/qstar/generated/version.h"),
  },
  command = qstar.cli {
    "tools/gen-version",
    qstar.output(0),
  },
  description = qstar.status("Generating C header version.h"),
}
```

```lua
qstar.run_target "smoke" {
  deps = {"//:app"},
  command = qstar.cli {qstar.target_file("//:app")},
  marker = "OK",
  description = qstar.status("Running smoke test app"),
}
```

Supported fields:

- `qstar.custom_target.description`
- `qstar.configure_file.description`
- `qstar.run_target.description`
- `qstar.stage.description`

Validation rules:

- Description은 한 줄 문자열이어야 한다.
- Empty string은 error다.
- Newline 포함 description은 error다.
- 240 byte를 넘는 description은 diagnostic으로 막는다.
- Raw string은 받지 않는다. `description = qstar.status("...")` 형태만 허용한다.
- `.qsm` helper는 `qstar.status(...)` 값을 반환할 수 있지만 graph declaration은 계속 금지된다.

## Progress Modes

| Option | Behavior |
| --- | --- |
| `--progress auto` | 기본값. TTY에서는 color-capable compact progress, non-TTY에서는 deterministic plain text |
| `--progress plain` | ANSI color 없는 deterministic progress text |
| `--progress off` | progress action line을 출력하지 않음 |

`--progress off`는 CI/log escape hatch다. Progress를 꺼도 final status, fatal error,
JSON diagnostic, requested command output은 필요한 범위에서 유지한다.

## Verbose And Schedule Trace

| Option | Contract |
| --- | --- |
| default | CMake-style progress line과 final summary만 표시 |
| `--verbose` | progress line은 유지하고 argv/cache/status detail을 추가 표시 |
| `--schedule-trace` | scheduler ready queue, action id, dependency edge 같은 internal trace를 추가 표시 |
| `--quiet` | 성공 progress를 줄이고 error/failure 중심으로 표시 |

`--verbose`와 `--schedule-trace`는 debugging surface다. 일반 output의 wording 계약을
흔들면 안 된다. Internal scheduler term은 이 두 mode에만 허용된다.

## Color Policy

`--color auto|always|never`는 text output에만 적용한다. JSON diagnostic object와
`qstar-action-diagnostic-v1` line에는 ANSI escape를 넣지 않는다.

Color rules:

- `warning:` prefix는 orange/yellow 계열로 표시한다.
- `error:` prefix는 bold red로 표시한다.
- 성공 final status는 green으로 표시할 수 있다.
- 주요 target/action heading은 필요할 때 bold를 사용할 수 있다.
- Non-TTY와 `--color never`에서는 color를 제거한다.

Compiler나 external tool이 stderr/stdout에 warning/error를 쓰면 QStar는 line 단위로
관찰하고 progress 중간에 즉시 표시한다. Terminal 출력에는 color policy에 맞춰
`warning:`/`error:` token만 색을 입히고, stdout/stderr log에는 color 없는 원문을
보존한다.

Example:

```txt
[ 23%] Building C object build/qstar/out/__lib/obj2.o
warning: src/lib/core.c:17: unused variable 'tmp'
[ 27%] Building C object build/qstar/out/__lib/obj3.o
```

## Removed From General Output

다음 표현은 일반 build progress에서 제거한다.

- legacy prepare/stage progress line
- `Status: compiling //:app`
- `schedule_action id=...`
- `build_action id=...`
- executor-internal state names

이 정보는 필요하면 `--verbose` 또는 `--schedule-trace`에서만 본다.

## Implementation Status

- Round Q102: action model에 `description` field를 추가했다.
- Round Q103: Stella executor가 action description으로 기본 progress를 렌더링한다.
- Round Q104: Stella executor가 child stdout/stderr를 line 단위로 관찰해 warning/error
  stream colorization을 적용한다.
- Round Q105: `qstar.status(...)`와 `description` field를 Lua DSL에 추가했다.
- Round Q105: Ninja emitter가 QStar description을 Ninja description으로 lower한다.
- Round Q107: action log, replay, last-failure가 action description을 보존한다.
