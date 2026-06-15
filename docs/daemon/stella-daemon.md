# Persistent Stella Daemon Workflow

이 문서는 Round Q143에서 persistent Stella daemon의 장기 계약을 고정했고, Round Q144에서
첫 experimental MVP 상태를 반영했으며, Round Q146에서 build output event stream을 추가했다.
Round Q147부터 daemon은 `state.db`와 `deps.db`의 in-memory snapshot을 먼저 사용한다. Round
Q148부터 daemon은 file watcher event로 authoring graph invalidation을 먼저 판단한다. Round
Q150부터 read-only query API가 있고, Round Q151 이후 `0.6.0-beta`에서는 documented beta
opt-in feature로 다룬다.
아직 stable/default-on CLI surface가 아니다. 목표는 QStar의 Lua DSL과 rich diagnostics는
유지하면서, 반복 build invocation마다 Lua eval, Graph IR load, plan cache load, dirty state
load를 처음부터 반복하지 않는 구조를 만드는 것이다.

```txt
status: documented-beta-opt-in-candidate
round: Q167
command namespace: qstar daemon
initial hosts: macOS and Linux over Unix domain sockets
deferred hosts: Windows named pipe
related: docs/perf/stella-plan-cache-design.md
related: docs/daemon-beta-readiness.md
```

Round Q164 adds a Windows build boundary: `_WIN32` hosts compile a daemon stub
instead of the Unix domain socket backend. `qstar daemon` and
`--use-daemon=always` report that Windows daemon support is deferred; the future
transport is a named pipe with Windows ACL validation.
Round Q167 strengthens Linux validation with an opt-in CI lane that records
`inotify` watcher status/events, daemon server logs, schedule traces, and
skip/fail reason artifacts.

## Decision

User-facing command namespace는 `qstar daemon`으로 한다. Q144 MVP는 foreground server와
explicit build client를 제공했고, Q150부터 read-only query helper도 같은 namespace에 둔다.
Q154부터는 `--start`/`--stop` background lifecycle도 같은 namespace에 둔다.

```sh
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --start
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --stop
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --serve
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --status
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query targets.list
qstar build //:app --use-daemon=auto --daemon-socket build/qstar/stella/daemon/qstar-daemon.sock
qstar build //:app --use-daemon=always --daemon-socket build/qstar/stella/daemon/qstar-daemon.sock
qstar build //:app --use-daemon=never
```

`qstar stella-daemon`은 채택하지 않는다. 이유는 세 가지다.

- `daemon`은 QStar CLI 안에서 service lifecycle을 다루는 더 일반적인 namespace다.
- 현재 daemon은 Stella executor용이지만, IDE/editor/AI integration은 같은 daemon
  lifecycle을 사용할 수 있다.
- user-facing command가 executor 내부 이름에 너무 강하게 묶이지 않는다.

`stella`라는 이름은 계속 generator/executor identity에 남는다. Daemon protocol의 service
name도 `stella`로 기록할 수 있지만, CLI command는 `qstar daemon`이다.

## Rollout Policy

초기 구현은 opt-in이다. Q143 설계안의 `--daemon auto|on|off` 이름은 Q144 MVP에서
`--use-daemon=auto|always|never`로 바뀌었다.

| Mode | 의미 |
| --- | --- |
| `--use-daemon=never` | daemon을 사용하지 않고 현재 Stella build path를 그대로 사용한다. |
| `--use-daemon=auto` | daemon 연결을 시도하고 실패하면 normal Stella build로 fallback한다. |
| `--use-daemon=always` | daemon 연결 또는 request가 실패하면 command failure로 처리한다. |

초기 beta에서는 `qstar build` 기본값을 즉시 daemon으로 바꾸지 않는다. CLI standalone build의
예측 가능성을 위해 normal Stella path를 유지하고, IDE/editor frontend와 performance
실험은 `--use-daemon=auto` 또는 explicit daemon lifecycle로 시작한다. 충분히 안정화되면 future
release에서 default policy를 다시 검토한다.

Fallback은 silent success가 아니다. `--verbose` 또는 `--schedule-trace`에서는 다음처럼
보여야 한다.

```txt
daemon status=unavailable reason=socket-missing fallback=stella
```

일반 output에서는 daemon fallback이 build result를 오염시키지 않아야 한다. 단,
`--use-daemon=always`에서는 fallback하지 않고 명확한 diagnostic을 낸다.

Q144 MVP는 같은 daemon process 안에서 Graph IR와 lowered plan cache 결과를 memory에
유지한다. Q146부터 daemon build response는 byte-count buffer가 아니라 event stream으로
전송되어 일반 Stella build와 같은 progress/warning/error output을 즉시 보여준다. Q147부터
daemon build는 `state.db`와 `deps.db`를 매 invocation마다 다시 parse하기 전에 process memory
snapshot을 먼저 사용한다. 성공 build 뒤에는 disk DB를 계속 writeback하므로 daemon crash 후
일반 Stella build도 같은 state에서 복구할 수 있다. Q148부터 authoring file watcher가 active인
경우 매 build request마다 전체 authoring fingerprint scan을 먼저 반복하지 않는다. Watcher가
불완전하거나 unavailable이면 기존 fingerprint scan으로 되돌아간다. Background start/stop
lifecycle, permission hardening은 후속 라운드다.

## Why A Daemon

현재 Stella는 다음 파일 기반 cache를 이미 가진다.

```txt
build/qstar/stella/manifest.json
build/qstar/stella/inputs.json
build/qstar/stella/graph.qsg
build/qstar/stella/actions.qsa
build/qstar/state/state.db
build/qstar/state/deps.db
```

이 구조는 no-op과 incremental build에서 큰 효과가 있다. 하지만 CLI process가 매번 새로
뜬다는 사실은 그대로다. Persistent daemon은 다음 비용을 process lifetime 안으로 끌어올린다.

- Lua runtime 초기화
- authoring input fingerprint read
- lowered action plan load
- compact state/deps DB load
- target/config/toolset lookup table materialization
- repeated path/label/description formatting cache
- file watcher event history

즉 daemon은 QStar 문법을 바꾸는 기능이 아니라 Stella 실행 경로의 residency model을 바꾸는
기능이다.

## Architecture

```txt
qstar CLI client
  -> Unix socket
  -> qstar daemon process
       -> workspace root guard
       -> loaded Graph IR / Stella Plan IR
       -> state.db / deps.db memory index
       -> file watcher invalidation queue
       -> Stella scheduler
       -> action log / replay writer
```

Daemon은 package root와 build directory 단위로 하나의 active instance를 가진다. 같은 source
tree라도 `-B build/a`와 `-B build/b`는 서로 다른 daemon identity로 본다. `-G ninja`는 daemon
대상이 아니다. Daemon은 Stella executor service다.

## Socket Layout

macOS와 Linux는 Unix domain socket을 먼저 지원한다.

```txt
build/qstar/stella/daemon/
  qstar-daemon.sock
  qstar-daemon.pid
  qstar-daemon.lock
  qstar-daemon.log
```

Socket은 build directory 아래에 둔다. Project root에 `.qstar.sock` 같은 파일을 만들지
않는다. Build directory가 지워지면 daemon은 stale로 간주된다.
`--start`는 같은 socket에 이미 daemon이 응답하면 `daemon already running`으로 실패한다.
`--stop`은 pid file과 hello response의 pid가 일치할 때만 종료한다. Pid file이 없는데 socket이
응답하는 경우에는 안전하지 않은 stop으로 보고 실패한다.
Q175부터 lifecycle regression은 `--start`, `--status`, `--stop`, duplicate start,
package-root/build-dir mismatch, stale socket cleanup, stale pid cleanup, stale lock cleanup을
같이 확인한다. Stale cleanup은 보수적이다. QStar는 owner-only socket directory 아래의
현재 사용자 소유 Unix socket/pid/lock sidecar만 정리하며, non-socket path는 절대 unlink하지 않는다.

권장 permission:

```txt
directory: 0700
socket:    0600
pid file:  0600
lock file: 0600
```

Windows named pipe는 deferred다. Future path는 다음 형태를 후보로 둔다.

```txt
\\.\pipe\qstar\<package-root-hash>\<build-dir-hash>
```

Windows에서는 path separator, ACL, process lifetime, console control event가 다르므로
Unix socket 설계가 곧바로 official contract가 되지 않는다.

## Protocol

Q146 implementation은 기존 request line protocol을 유지하고, build response만 event stream으로
바꾼다. Status/hello 같은 짧은 request는 기존 buffered response를 계속 쓸 수 있다.

Build response 시작:

```txt
qstar-daemon-stream-v1
```

Output event frame:

```txt
event <type> <byte-count>
<raw-bytes>
```

현재 type은 다음처럼 분류한다.

| Event | 의미 |
| --- | --- |
| `progress` | `[ 75%] Linking C executable app` 같은 CMake-style progress line |
| `diagnostic` | `warning:` 또는 `error:` line |
| `action` | `--verbose`/`--schedule-trace`에서 보이는 action/cache/scheduler line |
| `summary` | `qstar build`, `backend`, `root`, `status` 같은 build summary line |
| `output` | 그 밖의 원문 output |

Final event:

```txt
final <status-code>
```

CLI client는 event frame 자체를 사용자에게 노출하지 않고, payload만 기존 stdout으로 렌더링한다.
따라서 daemon build도 일반 Stella build처럼 보인다.

```txt
[ 50%] Building C object build/qstar/out/___app/obj0.o
[100%] Linking C executable build/qstar/out/___app/app
[100%] Built target app
status ok
```

Stream이 시작된 뒤 daemon이 죽으면 client는 partial output 뒤에
`qstar: daemon stream interrupted before final status`를 출력하고 command failure로 처리한다.
이 경우 `--use-daemon=auto`도 같은 build를 normal Stella로 조용히 재실행하지 않는다. Fallback은
연결 실패나 request 전송 실패처럼 build stream이 시작되기 전의 unavailable 상태에만 적용한다.

Protocol requirements:

- Requests are package-root scoped.
- Every request carries enough identity to reject build_dir/toolset graph/generator mismatch.
- The daemon never accepts shell-string commands from clients.
- The daemon does not expose arbitrary filesystem read/write RPC.
- Build output remains compatible with normal CLI output because the daemon streams the same
  Stella progress/color output as event payloads and the CLI client renders payloads only.

## Persistent Cache Model

Daemon memory state mirrors existing on-disk state.

| State | Source | Daemon behavior |
| --- | --- | --- |
| Authoring input fingerprints | `stella/inputs.json` and file watcher | Keep current in memory, invalidate on watcher event. |
| Lowered graph summary | `stella/graph.qsg` | Load once, reuse until fingerprint mismatch. |
| Lowered action plan | `stella/actions.qsa` | Load once, reuse for matching build request closure. |
| Dirty-check state | `state/state.db` | Q147 keeps compact action state in memory and writes it back after successful builds. |
| Discovered deps | `state/deps.db` | Q147 keeps depfile-discovered header state in memory and refreshes it on depfile change. |

`state.db` and `deps.db` remain the crash-recovery format. The daemon memory cache is a fast
mirror, not a replacement for the on-disk state.

With `--schedule-trace`, daemon builds show whether memory state and watcher invalidation were used:

```txt
daemon_watcher status=active backend=kqueue watches=18 incomplete=0 skipped_missing=0
daemon_watcher status=event backend=kqueue scope=authoring path=qstar.lua invalidation=graph reason=changed
daemon_watcher status=event backend=kqueue scope=input path=src/main.c invalidation=dirty-check reason=changed
dirty_state_memory status=miss reason=cold
dirty_state_memory status=hit entries=12
dirty_state_memory status=writeback entries=12
deps_memory status=hit entries=4
deps_memory status=writeback entries=4
```
| Debug dumps | opt-in only | Do not create unless requested by env/trace. |

If watcher state is uncertain, the daemon must fall back to the same fingerprint checks that a fresh
CLI invocation performs. Watcher events are an acceleration signal, not the only correctness source.

## File Watcher

Initial watcher backends:

| Host | Backend | Status |
| --- | --- | --- |
| macOS | kqueue vnode events | MVP implemented |
| Linux | inotify | MVP implemented, opt-in CI validation artifact |
| Windows | named pipe plus ReadDirectoryChangesW | Deferred |

Watcher scope:

- `qstar.lua`
- imported `.qst`
- imported `.qsm`
- source/header paths in the requested graph
- generated inputs and generated outputs that already exist when the watcher set is refreshed

The watcher only accepts package-relative paths that QStar already considers inside the package root.
Workspace root parents are never watched. Missing generated outputs are skipped during the first
registration and picked up after a successful build refresh when possible. If a watcher cannot be
registered, the daemon marks the watcher incomplete and keeps the older authoring fingerprint scan as
fallback. If a source path escapes the package root, existing QStar validation rejects it before
daemon use.

Watcher invalidation classes:

| Event | Invalidation |
| --- | --- |
| authoring file changed | Drop Graph IR and Stella Plan IR. |
| source/header changed | Keep graph, mark affected actions dirty. |
| generated output deleted | Mark producing/consuming action dirty. |
| build_dir deleted | Stop daemon or mark stale. |
| toolset graph input changed | Drop Graph IR and Stella Plan IR. |

The Q148 MVP does not bypass Stella dirty checking for source/header changes. Source/header watcher
events are traced as `invalidation=dirty-check`; the existing compact `state.db`/`deps.db` path still
decides which action is dirty. Event loss, overflow, or backend errors are conservative graph
invalidation events.

## Security

Daemon security is part of the contract, not an afterthought.

- The daemon serves exactly one package root and one effective build directory.
- Every path received from a client is normalized and checked against the package root or build dir.
- Socket directory and socket file must be owner-only.
- If socket owner does not match the current user, the CLI refuses to connect.
- The daemon must reject requests with a mismatched package root, build dir, qstar version, or
  protocol version.
- The daemon never accepts arbitrary command execution RPC. It only executes actions produced by the
  validated QStar graph.
- Existing socket cleanup is conservative: QStar only removes a socket file after owner/permission
  checks pass and a connect probe proves no listener is alive. Non-socket files are never unlinked.
- Remote access is out of scope. A future remote daemon must be an explicit opt-in feature with
  authentication and audit logging.

## AI And IDE Integration

The daemon is the long-term bridge between QStar and IDE/editor/AI workflows.

Q150 implements the first read-only query surface:

```txt
qstar daemon --socket path --query method
```

Implemented read methods:

```txt
hello
workspace.info
targets.list
diagnostics.list
compile_commands.path
build.summary
```

Responses are JSON. `targets.list` intentionally reuses the existing `qstar-targets-v1` schema from
`qstar list-targets --format json`; the other methods use `qstar-daemon-read-v1`. The method
contract is documented in `docs/contracts/daemon-read-api.md`.

Deferred read/action APIs:

```txt
target.explain
build.status
action.log
replay.plan
build.request
test.request
clean.request
cancel.request
```

Default IDE/AI capability is read-only. Mutating actions such as build/test/clean stay on the
existing command path for now and require an explicit user action or a trusted local client policy.
File edits are not daemon responsibilities. They belong to the editor or AI tool, with separate
preview/apply/audit rules.

Read API examples:

```sh
qstar --file qstar.lua daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query hello
qstar --file qstar.lua -B build/qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query targets.list
qstar --file qstar.lua -B build/qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query compile_commands.path
```

## CLI Fallback

Fallback behavior:

| Condition | `--use-daemon=auto` | `--use-daemon=always` |
| --- | --- | --- |
| socket missing | normal Stella build | fail |
| protocol mismatch | normal Stella build | fail |
| stale daemon socket | cleanup then normal Stella build | fail after cleanup attempt |
| request rejected due to root mismatch | fail | fail |
| daemon build action fails | fail | fail |
| daemon crashes mid-build | fail, leave last-failure if available | fail |

Root mismatch is never a fallback case because falling back could hide a security problem.

## Implementation Rounds

Recommended future implementation order:

1. Q144: foreground `qstar daemon --serve`, `--status`, `qstar build --use-daemon=...` forwarding.
2. Q146: CLI progress streaming instead of response-at-end forwarding.
3. Q147: in-memory `state.db` and `deps.db` snapshots with disk write-back.
4. Q148: file watcher invalidation for authoring files and source/header paths.
5. Q150: read-only `hello`, `workspace.info`, `targets.list` protocol.
6. Q153: owner-only socket directory/file checks, protocol mismatch diagnostics, and identity hard
   rejection.
7. Q154: background `--start`/`--stop`, pid/lock file, and duplicate start diagnostic.
8. Q175: lifecycle beta seal with stale socket/pid/lock cleanup regression,
   package-root mismatch hard reject, Linux inotify status artifact, and Windows named pipe
   deferred status.
9. IDE/AI read-only API surface and audit log.
10. Windows named pipe design refresh and native validation.

## Non-Goals

- No mandatory long-running service for normal QStar use.
- No package manager or dependency resolver.
- No remote daemon in the first implementation.
- No shell-string execution escape hatch.
- No new QStar authoring syntax.
- No Windows official support until a native named-pipe/process validation lane exists.
