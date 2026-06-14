# Stella Daemon

Stella daemon은 QStar의 장기 성능 구조다. Round Q148 기준으로 foreground server, build
client, streaming build output, in-memory dirty/deps state snapshot, file watcher invalidation이
experimental MVP로 들어왔지만, stable public surface는 아니다. 정본 설계 문서는
`docs/daemon/stella-daemon.md`다.

## 명령 이름

명령 namespace는 `qstar daemon`으로 정한다.

```sh
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --serve
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --status
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query targets.list
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
즉시 표시한다. Q147부터 daemon은 `state.db`와 `deps.db`의 in-memory snapshot을 먼저 사용한다.
성공 build 뒤에는 같은 state를 disk DB로 writeback하므로 daemon crash 뒤에도 일반 Stella build가
복구할 수 있다. Q148부터 daemon은 macOS `kqueue` 또는 Linux `inotify` file watcher가 active인
경우 authoring input stat scan을 매 request마다 반복하지 않고, watcher event로 graph reload
여부를 먼저 결정한다. Watcher가 unavailable이거나 incomplete이면 기존 fingerprint scan으로
fallback한다. 장기 persistent daemon은 여기에 더해 다음 상태를 process memory에 유지한다.

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

## In-Memory State

Daemon build는 같은 build directory의 `state.db`와 `deps.db`를 process memory에 유지한다.
첫 request는 disk DB를 읽고, 이후 같은 identity에서는 memory snapshot을 먼저 사용한다.

`--schedule-trace`에서는 다음 line으로 확인할 수 있다.

```txt
dirty_state_memory status=hit entries=12
deps_memory status=hit entries=4
dirty_state_memory status=writeback entries=12
deps_memory status=writeback entries=4
daemon_watcher status=active backend=kqueue watches=18 incomplete=0 skipped_missing=0
daemon_watcher status=event backend=kqueue scope=authoring path=qstar.lua invalidation=graph reason=changed
daemon_watcher status=event backend=kqueue scope=input path=src/main.c invalidation=dirty-check reason=changed
```

Disk DB는 계속 canonical crash-recovery format이다. Memory snapshot은 daemon fast path일 뿐,
`qstar why-rebuild`, `qstar action-log`, `qstar replay`, `qstar last-failure`의 동작을 바꾸지 않는다.

Source/header watcher event는 dirty-check를 대체하지 않는다. 이벤트는 `invalidation=dirty-check`로
기록되고, 실제 rebuild 판단은 기존 compact `state.db`/`deps.db` 경로가 계속 담당한다. Authoring
event, watcher overflow, backend 오류는 conservative graph reload로 처리한다.

## 보안 원칙

- daemon은 하나의 package root와 하나의 build directory만 담당한다.
- 모든 path는 package root 또는 build directory 아래인지 다시 검사한다.
- socket directory와 socket permission은 owner-only여야 한다.
- stale socket cleanup은 pid liveness와 handshake failure를 둘 다 확인한 뒤에만 한다.
- remote access는 scope 밖이다.
- daemon은 validated QStar graph에서 나온 action만 실행한다.

## IDE/AI 연동

Daemon은 IDE/editor/AI frontend가 QStar context를 빠르게 읽는 연결점이 될 수 있다.

Round Q150부터 첫 read-only query API가 구현되어 있다.

```sh
qstar --file qstar.lua -B build/qstar daemon --socket /tmp/qstar.sock --query hello
qstar --file qstar.lua -B build/qstar daemon --socket /tmp/qstar.sock --query workspace.info
qstar --file qstar.lua -B build/qstar daemon --socket /tmp/qstar.sock --query targets.list
qstar --file qstar.lua -B build/qstar daemon --socket /tmp/qstar.sock --query diagnostics.list
qstar --file qstar.lua -B build/qstar daemon --socket /tmp/qstar.sock --query compile_commands.path
qstar --file qstar.lua -B build/qstar daemon --socket /tmp/qstar.sock --query build.summary
```

구현된 read API:

- `workspace.info`
- `targets.list`
- `diagnostics.list`
- `compile_commands.path`
- `build.summary`

`hello`, `workspace.info`, `diagnostics.list`, `compile_commands.path`, `build.summary`는
`qstar-daemon-read-v1` JSON을 반환한다. `targets.list`는 기존
`qstar list-targets --format json`과 같은 `qstar-targets-v1` schema를 반환한다.

Deferred read/action API:

- `target.explain`
- `build.status`
- `action.log`
- `replay.plan`

- `build.request`
- `test.request`
- `clean.request`
- `cancel.request`

기본 권한은 read-only가 맞다. build/test/clean 같은 action은 사용자 승인 또는 신뢰된 local
client policy가 있어야 한다.

## 관련 문서

- `docs/daemon/stella-daemon.md`
- `docs/contracts/daemon-read-api.md`
- `docs/perf/stella-plan-cache-design.md`
- [Performance Gates](performance-gates.md)
- [Backends](backends.md)
