# Progress Output

이 문서는 QStar 0.5 UI line의 progress 출력 계약이다. Stella와 Ninja는 같은
user-facing action description을 사용하고, action-log/replay/last-failure CLI도 같은
description metadata를 보존한다.

## 기본 형식

QStar build progress는 CMake-style action line을 사용한다.

```txt
[  8%] Building C object build/qstar/out/__core/obj0.o
[ 23%] Building CXX object build/qstar/out/__ui/obj4.o
warning: src/ui/view.cpp:42: unused variable 'tmp'
[ 51%] Linking C static library libcore.a
[ 63%] Linking C shared library libplugin.dylib
[ 75%] Linking CXX executable app
[100%] Built target app
```

Percent는 `[ 75%]`처럼 3칸 폭으로 맞춘다. Percent 뒤에는 legacy scheduler stage wording이나
상태가 아니라 build action description이 온다.

## 일반 출력에서 숨기는 것

일반 build output에는 다음을 표시하지 않는다.

- prepare/stage progress line
- `Status: compiling ...`
- `schedule_action id=...`
- `build_action id=...`
- executor-internal state name

이 정보는 `--verbose`나 `--schedule-trace`에서만 본다.

## Progress 옵션

| 옵션 | 의미 |
| --- | --- |
| `--progress auto` | 기본값. TTY에서는 color-capable progress, non-TTY에서는 plain text |
| `--progress plain` | color 없는 deterministic text |
| `--progress off` | progress action line을 끔 |

`--progress off`는 CI/log escape hatch다. Progress를 꺼도 final status와 error는 필요한
범위에서 유지된다.

## Verbose와 schedule trace

- 기본 모드: CMake-style progress와 final summary.
- `--verbose`: argv, cache, action status detail을 추가한다.
- `--schedule-trace`: scheduler ready queue, dependency edge, action id 같은 내부 trace를
  추가한다.
- `--quiet`: 성공 progress를 줄이고 error/failure 중심으로 출력한다.

## Color 정책

`--color auto|always|never`는 text output에만 적용한다. JSON diagnostic에는 color를 넣지
않는다.

- `warning:` prefix는 orange/yellow 계열.
- `error:` prefix는 bold red.
- 성공 final status는 green 가능.
- non-TTY와 `--color never`에서는 ANSI escape를 제거.

Compiler나 external tool의 stdout/stderr warning/error line은 progress 중간에 즉시
표시한다. Terminal 출력에는 color policy에 맞춰 `warning:`/`error:` token만 색을
입히고, stdout/stderr log에는 color 없는 원문을 보존한다.

```txt
[ 23%] Building C object build/qstar/out/__lib/obj2.o
warning: src/lib/core.c:17: unused variable 'tmp'
[ 27%] Building C object build/qstar/out/__lib/obj3.o
```

## qstar.status

사용자 정의 action은 `qstar.status(...)`로 status description을 줄 수 있다.

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

`qstar.status(...)`는 한 줄 description만 허용한다. Empty string, newline, 240 byte를
넘는 description은 diagnostic 대상이다. Raw string은 받지 않으며 반드시
`description = qstar.status("...")` 형태를 써야 한다.

적용 가능한 field:

- `qstar.custom_target.description`
- `qstar.configure_file.description`
- `qstar.run_target.description`
- `qstar.stage.description`

`.qsm` helper는 `qstar.status(...)` 값을 반환할 수 있지만 graph declaration은 계속 금지된다.

## Action Description IR

Stella action plan은 사용자-facing description을 별도 field로 가진다.
`qstar explain`, `qstar dry-run`, `--verbose`, `--schedule-trace`에서 다음 line을 확인할 수
있다.

```txt
action_description id=//:app:compile:0 text="Building C object build/qstar/out/___app/obj0.o"
action_description id=//:app:link:0 text="Linking C executable build/qstar/out/___app/app"
```

`qstar.group`은 progress action에서 제외된다.

```txt
progress_action label=//:all include=no reason=group
```

## Log와 replay

Action description은 action log, replay, last-failure에도 `description=` metadata로 남는다.

```txt
qstar action-log //:app:compile:0
description='Building C object build/qstar/out/___app/obj0.o'

qstar replay //:app:compile:0
description='Building C object build/qstar/out/___app/obj0.o'

qstar last-failure
description='Running smoke test app'
```

Stella executor에서 성공/skip action의 물리 `.log` 파일 존재는 public contract가 아니다.
성공 action은 compact state와 lowered action plan, 현재 graph에서 필요 시 재구성된다. 실패
action과 `last-failure` replay는 재현성을 위해 즉시 물리 파일로 기록한다.

일반 progress는 CMake-style line만 보여주고, 디버깅 명령은 같은 description을 사용해 action
context를 다시 보여준다.
