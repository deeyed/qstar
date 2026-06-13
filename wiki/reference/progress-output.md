# Progress Output

이 문서는 QStar 0.5 UI line의 progress 출력 계약이다. Round Q101에서는 구현보다 먼저
Stella와 Ninja가 따라야 할 user-facing wording을 고정한다.

## 기본 형식

QStar build progress는 CMake-style action line을 사용한다.

```txt
[  8%] Building C object build/qstar/out/__core/obj0.o
[ 23%] Building CXX object build/qstar/out/__ui/obj4.o
warning: src/ui/view.cpp:42: unused variable 'tmp'
[ 51%] Linking C static library libcore.a
[ 75%] Linking CXX executable app
[100%] Built target app
```

Percent는 `[ 75%]`처럼 3칸 폭으로 맞춘다. Percent 뒤에는 `Stage N`이나 scheduler
상태가 아니라 build action description이 온다.

## 일반 출력에서 숨기는 것

일반 build output에는 다음을 표시하지 않는다.

- `Stage 1: prepare`
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

Compiler warning은 가능하면 progress 중간에 표시한다.

```txt
[ 23%] Building C object build/qstar/out/__lib/obj2.o
warning: src/lib/core.c:17: unused variable 'tmp'
[ 27%] Building C object build/qstar/out/__lib/obj3.o
```

## qstar.status 계획

사용자 정의 action은 future DSL hook으로 status description을 줄 수 있다.

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

`qstar.status(...)`는 한 줄 description만 허용한다. Empty string, newline, 너무 긴
description은 diagnostic 대상이다.

## Action Description IR

Round Q102부터 Stella action plan은 사용자-facing description을 별도 field로 가진다.
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
