# Stella Daemon

Stella daemon은 QStar의 장기 성능 구조다. Round Q146 기준으로 foreground server와 build
client, 그리고 streaming build output이 experimental MVP로 들어왔지만, stable public surface는 아니다. 정본 설계 문서는
`docs/daemon/stella-daemon.md`다.

## 명령 이름

명령 namespace는 `qstar daemon`으로 정한다.

```sh
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --serve
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --status
qstar build //:app --use-daemon=auto --daemon-socket build/qstar/stella/daemon/qstar-daemon.sock
qstar build //:app --use-daemon=always --daemon-socket build/qstar/stella/daemon/qstar-daemon.sock
qstar build //:app --use-daemon=never
```

`qstar stella-daemon`은 채택하지 않는다. Daemon은 현재 Stella executor를 빠르게 만들기 위한
service지만, CLI 명령은 QStar service lifecycle을 나타내는 `daemon`이 더 넓고 안정적이다.

## 목적

현재 Stella는 `actions.qsa`, `state.db`, `deps.db` 같은 build directory 내부 cache를 사용한다.
Q144 MVP는 Graph IR와 lowered plan cache 결과를 process memory에 유지한다. Q146부터 build
response는 event stream으로 전달되어 일반 Stella build와 같은 progress/warning/error output을
즉시 표시한다. Authoring input mtime/size가 바뀌면 graph를 다시 load하고, 일반 source/header
변경은 기존 Stella dirty-check state가 처리한다. 장기 persistent daemon은 여기에 더해 다음
상태를 process memory에 유지한다.

- Lua runtime
- Graph IR와 Stella Plan IR
- target/config/profile lookup table
- file watcher invalidation queue
- compact dirty state와 discovered dependency index
- progress/action description formatting cache

이 구조의 목적은 같은 project를 반복 build할 때 Lua eval, graph load, plan cache load를 매번
처음부터 반복하지 않는 것이다.

## Host 지원

| Host | 계획 |
| --- | --- |
| macOS | Unix domain socket 우선 |
| Linux | Unix domain socket 우선 |
| Windows | named pipe deferred |

Socket은 project root가 아니라 build directory 아래에 둔다.

```txt
build/qstar/stella/daemon/qstar-daemon.sock
```

## Fallback

초기 구현은 opt-in이어야 한다.

| Mode | 동작 |
| --- | --- |
| `--use-daemon=never` | 현재 Stella build path를 그대로 사용 |
| `--use-daemon=auto` | daemon을 시도하고 실패하면 normal Stella build로 fallback |
| `--use-daemon=always` | daemon 연결 또는 request 실패 시 command 실패 |

Package root mismatch 같은 보안 오류는 fallback 대상이 아니다.

Build stream이 시작된 뒤 daemon이 죽으면 client는 partial output 뒤에
`qstar: daemon stream interrupted before final status`를 출력하고 실패한다. 이 경우
`--use-daemon=auto`도 normal Stella build로 조용히 재실행하지 않는다. Fallback은 daemon 연결
실패처럼 build stream이 시작되기 전의 unavailable 상태에만 적용한다.

## Streaming Output

Daemon build output은 client-visible raw protocol이 아니라 내부 event stream이다.

```txt
qstar-daemon-stream-v1
event progress <bytes>
event diagnostic <bytes>
event action <bytes>
event summary <bytes>
event output <bytes>
final <status-code>
```

CLI는 frame을 숨기고 payload만 렌더링한다. 그래서 daemon build도 일반 Stella build와 같은
형식으로 보인다.

```txt
[ 75%] Linking C executable app
warning: src/main.c:17: unused variable 'tmp'
status ok
```

`--color never`와 `--progress off` 같은 기존 build option은 daemon 경로에서도 동일하게 적용된다.

## 보안 원칙

- daemon은 하나의 package root와 하나의 build directory만 담당한다.
- 모든 path는 package root 또는 build directory 아래인지 다시 검사한다.
- socket directory와 socket permission은 owner-only여야 한다.
- stale socket cleanup은 pid liveness와 handshake failure를 둘 다 확인한 뒤에만 한다.
- remote access는 scope 밖이다.
- daemon은 validated QStar graph에서 나온 action만 실행한다.

## IDE/AI 연동

Daemon은 IDE/editor/AI frontend가 QStar context를 빠르게 읽는 연결점이 될 수 있다.

후보 read API:

- `workspace.info`
- `targets.list`
- `target.explain`
- `diagnostics.list`
- `build.status`
- `action.log`
- `replay.plan`

후보 action API:

- `build.request`
- `test.request`
- `clean.request`
- `cancel.request`

기본 권한은 read-only가 맞다. build/test/clean 같은 action은 사용자 승인 또는 신뢰된 local
client policy가 있어야 한다.

## 관련 문서

- `docs/daemon/stella-daemon.md`
- `docs/perf/stella-plan-cache-design.md`
- [Performance Gates](performance-gates.md)
- [Backends](backends.md)
