# QStar AI Index

이 문서는 Codex 같은 AI agent가 QStar project를 빠르게 이해하고 안전하게 수정하기 위한
첫 진입점이다. 사람이 처음 배우는 문서는 `README.md`와 `getting-started.md`에서
시작하고, agent는 이 파일을 먼저 읽은 뒤 필요한 reference로 이동한다.

## 1. QStar의 역할

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다.
현재 runtime은 built-in `c`, `cxx`, `asm` provider namespace를 preloaded registry로
다루며, public `lang.c`, `lang.cxx`, `lang.asm` syntax는 계속 유지한다. QStar는 build
graph, command plan, local executor, stage/package, run smoke, lint/LSP authoring UX를
담당한다. C/C++/ASM 외 언어의 의미론은 외부 compiler와 GLP source unit 또는 object artifact
bridge가 맡는다.

Generic Language Provider(GLP)는 이 경계를 정식 provider surface로 확장한다. 현재 runtime은
`qstar.use_language("zig")`, provider namespace toolset, 동적
`lang.zig` activation gate, provider-defined option schema, `qstar.source(...)` 기반 source
unit object lowering을 제공한다. Provider implementation의 lowering function은 `command`,
`inputs`, `outputs`, `depfile` action template을 만들고, Stella와 Ninja는 이 template을 같은
backend contract로 실행한다. `zig`, `rust`, `cuda` provider는 installed standard provider
bundle에 포함되고, project-local `qstar/languages/<id>/<id>.qsm`이 있으면 그 manifest가
우선한다. Provider 문법의 정본은 `wiki/reference/language-providers.md`다.

QStar가 하지 않는 일:

- compiler frontend/backend 내부 API 호출
- 외부 언어 source 의미 해석
- package fetch, registry, lockfile resolution
- domain-specific builtin target 제공
- shell-string command 실행

## 2. 반드시 지킬 authoring surface

- root entry는 `qstar.lua` 하나다.
- project metadata는 `qstar.project { name, version, root = "." }`로 둔다.
- subdir fragment는 `<folder>/<folder>.qst`다.
- `qstar.import_file("path.qst")`는 package-root 기준 `.qst` graph fragment를 읽는다.
- `qstar.import_module("folder/path")`는 `folder/path/path.qsm` helper module table을 읽는다.
- `qstar.import_module`에는 `.qsm` 파일이 아니라 폴더만 넘긴다.
- `.qsm` 안에서는 target/toolset/project 같은 graph declaration이 금지된다.
- 반복 option은 `qstar.config`와 target `configs = {...}`로 공유한다.
- 반복 path/list/table 조립은 `qstar.join`, `qstar.copy`, `qstar.append`, `qstar.merge`,
  `qstar.extend`를 쓴다.
- 사용자 정의 build step 문구는 `description = qstar.status("...")`로 지정한다.
  이 field는 `qstar.custom_target`, `qstar.configure_file`, `qstar.run_target`,
  `qstar.stage`에서 지원된다. Raw string, empty string, newline, 240 byte 초과 문자열은
  diagnostic 대상이다.
- Makefile식 `$VAR` 문자열 치환은 없다. Lua `local` 변수와 helper function을 쓴다.
- legacy qs fragment suffix와 `qstar.workspace`는 제거된 surface다.
- 산출물 기본 위치는 `build/qstar`다.
- generated action output 기본 root는 `generated`이고, `qstar.project.generated_dir`로
  package-relative generated root를 바꿀 수 있다.
- QStar가 직접 지원하지 않는 언어 source도 활성화된 GLP provider가 `units.*.suffixes`로 등록하면
  raw string `sources` classification에 참여한다. 예를 들어 `qstar.use_language("zig")` 이후
  `sources = {"src/main.zig"}`는 consuming target 소유의 provider object output으로 낮아진다.
  `zig.object("src/main.zig", {...})` 같은 helper는 source-local option이나 suffix 충돌 해소용
  명시 경로다. 더 세밀한 외부 compiler 호출이 필요하면 `qstar.custom_target`과
  `qstar.output(path, {format = "object"})` bridge도 계속 사용할 수 있다.
- GLP provider는 project-local `qstar/languages/<id>/<id>.qsm` manifest 또는 installed
  standard bundle `share/qstar/languages/<id>/<id>.qsm` manifest와 `provider.lua`
  implementation을 가진다. Manifest는 `qstar.language_provider { api = "qstar.lang/1", ... }`
  schema로 검증되고, implementation은 제한 provider sandbox에서 로드된다. 사용자는
  `qstar.use_language("<id>")`가 반환한 exported helper table을 통해 `zig.tools`,
  `rust.options`, `cuda.object` 같은 helper를 사용한다. `lang.<namespace>`와 raw provider source
  classification은 provider activation 이후에만 유효하며, provider-defined `options` schema가
  unknown option, string, bool, list, enum, default metadata를 검증한다. Optional `scaffold` schema는
  `api = "qstar.scaffold/1"`, default tools/options, shape files/targets/fragments를
  package-relative declarative data로 검증하며, `qstar init`은 primary provider shape를
  실제 `qstar.lua`, source file, workspace fragment로 materialize한다. 같은 manifest의
  `qstar.use_language(...)`는 root와 fragment에서 idempotent하게 helper table을 재사용한다.
  Source unit lowering은 `ctx.tool`,
  `ctx.input`, `ctx.output`, `ctx.option`으로 작성한 action template을 Stella/Ninja 양쪽에
  공유한다.
- CLI `-B path`는 `qstar.project.build_dir`보다 우선한다.
- CLI `-G auto`는 현재 `stella`로 resolve된다.
- CLI `-G ninja build [label]`은 C/C++/ASM compile, `qstar.configure_file`,
  `qstar.custom_target`, staticlib, sharedlib, executable/test link, `qstar.run_target`,
  `qstar.group` phony graph를 Ninja로 실행한다. `stage`와 `install`은 copy와 manifest를
  QStar가 처리하지만, 참조 target artifact는 effective generator로 먼저 build한다.
  `sharedlib`는 macOS platform context에서 `.dylib`, Linux platform context에서 `.so`를 만들며,
  sharedlib dependency를 link하는 executable/test/sharedlib에는 build-tree 실행용
  `@loader_path`/`$ORIGIN` rpath를 자동 추가한다. Windows sharedlib는 runtime `.dll`,
  import `.lib`, PDB/debug artifact 정책이 아직 없으므로 deferred diagnostic으로 거부한다.
- `make qstar-ninja-backend-parity-tests`는 staticlib, sharedlib, sharedlib-linked
  executable/test, generated, configure_file, run_target, sharedlib stage/install producer integration,
  action-log/replay, Windows sharedlib diagnostic, `.ninja_log`/`.ninja_deps` root
  pollution 방지를 확인한다.
- `qstar emit-ninja [label]`은 `build/qstar/ninja/build.ninja`와 policy-controlled
  `compile_commands.json`을 생성한다.
- compile database 기본 위치는 `build/qstar/compile_commands.json`이다.
- QStar repository 자체는 Makefile을 canonical bootstrap/release build path로 유지하면서
  root `qstar.lua` self-host graph를 제공한다. `make qstar-self-host-tests`는
  Makefile-built binary와 Stella/Ninja self-host binary의 `--version` 일치, `//:qstar`
  build, `//:self_host` smoke, compile database, Ninja root pollution 방지를 확인한다.
- Public beta runtime package는 `make qstar-public-beta-release-tests`로 만든다. 이 gate는
  installed binary version, installed wiki/manpages, macOS codesign, prefix-style
  tarball layout, `SHA256SUMS`, VSCode `.vsix` 미포함 정책을 확인한다. GitHub Wiki
  mirror는 `tools/sync-github-wiki.sh`로 수행한다.
- GitHub release 생성 뒤에는 `make qstar-public-beta-download-smoke`를 실행해 실제 uploaded
  asset을 다시 다운로드하고 `SHA256SUMS`, `/tmp` extract, `qstar --version`, installed
  docs/manpages, macOS codesign을 확인한다. Linux x86_64 download smoke는 Linux host에서
  `file(1)`, `ldd(1)`, wiki home, Lua reference, manpage source/render report도 보존한다.
  0.6 post-release 기록은 `docs/qstar-v0.6-post-release-smoke.md`에 둔다.
- Progress output contract는 `docs/progress-output.md`와 `wiki/reference/progress-output.md`에
  둔다. QStar 0.5 UI line은 `[ 75%] Linking CXX executable app` 같은 CMake-style
  action description을 일반 출력으로 사용하고, legacy scheduler stage wording, `Status: ...`,
  `schedule_action`, `build_action` 같은 내부 trace는 `--verbose`나 `--schedule-trace`로
  제한한다. Warning은 `warning:` prefix를 orange/yellow, error는 bold red로 표시한다.
  `qstar action-log`, `qstar replay`, `qstar last-failure`도 같은 action description을
  `description=` metadata로 보존한다. Stella 성공/skip action의 물리 `.log` 파일 존재는
  public contract가 아니며, 필요 시 compact state와 현재 graph에서 lazy 재구성될 수 있다.
- 0.7 feature beta 판단과 v1 blocker 정리는 `docs/qstar-v0.7-readiness.md`에 둔다.
  Q169 기준 이 문서는 local `make check`, self-host, large performance, public beta
  package smoke 결과를 묶어 `v0.7.0-beta` tag/release 준비 라운드로 넘겨도 된다고 판단한다.
  다음 feature line은 `0.7.0-beta`가 맞고, `0.6.x-beta`는 release/package/doc
  hotfix용 patch line으로 남긴다. 0.7의 핵심은 Windows alpha/platform policy, daemon
  readiness, Linux/macOS asset gate hardening, Stella/Ninja/Linux timing refresh다.
- 0.8 feature line 판단은 `docs/qstar-v0.8-readiness.md`에 둔다. Q177 기준 다음 방향은
  Windows beta path와 Windows `qstar.sharedlib` `.dll`/import `.lib` implementation이다.
  `0.7.x-beta`는 release/package/docs/perf-gate hotfix line으로 남기고, `0.8.0-beta`는
  Windows process/event portability, real-host `.exe`/static `.lib`, Windows sharedlib
  artifact parity, daemon default-prep hardening을 맡는다. Daemon은 계속 opt-in이고,
  package resolver/registry/lockfile/fetch policy는 QStar core 밖에 둔다.
- 0.6 beta release 판단과 `0.6.1-beta` public prerelease line은
  `docs/qstar-v0.6-readiness.md`에 둔다. 이 문서는 daemon beta opt-in, self-host,
  Stella/Ninja benchmark, Linux x86_64 beta asset, Windows status, package smoke,
  version policy, deferred surface를 요약한다. 0.5 line의 연속 기록은
  `docs/qstar-v0.5-readiness.md`에 남긴다.
- Linux host 지원은 Round Q157 기준 public beta asset 단계다.
  `linux-x86_64` release tarball은 Ubuntu release workflow 또는 clean Linux x86_64 host
  산출물만 사용하며, extracted tarball smoke, explicit Ninja backend parity, medium
  performance artifact upload가 release gate다.
  `make qstar-linux-validation-tests`는 portable path/process, Linux depfile 후보,
  generated_dir, install layout, docs/manpage smoke를 묶는다. macOS에서는 제한된 path
  smoke다.
- Round Q109/Q156/Q157의 Linux validation workflow는 `.github/workflows/linux-validation.yml`이다.
  `ubuntu-latest`에서 gcc/clang matrix를 돌리고 `QSTAR_LINUX_VALIDATION_CC`로 실제 depfile
  compiler lane을 고정한다. 각 lane은 Ninja를 설치하고 `make all`, `make check`,
  `make qstar-linux-validation-tests`, `make qstar-ninja-backend-parity-tests`,
  `make qstar-generic-dsl-backend-parity-tests`, install docs/man smoke를 실행한다. gcc lane은
  `QSTAR_RELEASE_PLATFORM=linux-x86_64 tools/package-public-beta.sh`로 ELF x86-64,
  `ldd`, installed/extracted docs/wiki/manpages, `SHA256SUMS`를 확인하는 publish 없는
  dry-run을 수행하고, `qstar-linux-x86_64-release-candidate-dry-run` artifact를 업로드한다.
  Round Q142부터 같은 gcc/clang lane은
  `tests/medium-project-performance.sh`도 실행해 Stella/Ninja medium timing artifact를
  업로드한다. Linux trace는
  `medium_project_gate scheduler runner=posix_spawn event_wait=poll`을 포함해야 한다.
  Large synthetic performance는 기본 push/PR lane이 아니라 `workflow_dispatch` 전용
  `large-performance-report` job에서 report-only artifact로 수집한다.
  Linux asset upload는 `workflow_dispatch`의 `publish_linux_asset=true`,
  `release_tag=v0.7.0-beta` opt-in job에서만 수행하며,
  `tools/publish-github-release-asset.sh`가 기존 `SHA256SUMS`에 Linux checksum을 병합한다.
  Upload 직후에는 `make qstar-public-beta-download-smoke`를 다시 실행하고
  `download-smoke-linux-x86_64` artifact에 checksum, `file`, `ldd`, docs/wiki, manpage
  reports를 남긴다. Q171부터 hosted release verification은 gcc/clang install-smoke
  artifacts, medium `perf_summary status=ok`, downloaded `contents.txt`,
  `download-smoke.log`, and `linux-hosted-release-decision.txt`도 보존한다. Linux asset은
  decision file이 `linux_release_asset status=published`와 `download_smoke=ok`를 기록한
  뒤 release-backed로 본다. Q171 hosted run
  `https://github.com/deeyed/qstar/actions/runs/27507809666`은 `v0.7.0-beta`
  Linux x86_64 asset을 publish하고 download smoke까지 통과했으므로, 이 release의
  `qstar-v0.7.0-beta-linux-x86_64.tar.gz`는 release-backed beta asset이다.
  Linux daemon socket smoke도 기본 push/PR lane이 아니라 `workflow_dispatch`의
  `daemon_socket_smoke=true` opt-in job에서 검증한다.
- Windows host 지원은 아직 official support가 아니다. Round Q114/Q158/Q159 기준
  `make qstar-windows-prep-tests`가 path/process/MSVC response-file 준비 규칙을 묶고,
  `make qstar-windows-native-alpha-tests`가 제한 smoke를 제공한다.
  `.github/workflows/windows-validation.yml`은 `workflow_dispatch` 전용 manual native CI
  alpha다. Q172 기준 baseline lane은 `msys2-ucrt64-gcc`이며, job 이름은
  `windows alpha / msys2-ucrt64-gcc baseline`이다. MSYS2 UCRT64에서 `make all CC=gcc`,
  `qstar --version`, native alpha smoke,
  Windows prep, install docs/man smoke를 실행하고 `qstar-windows-native-alpha` artifact로 로그를 남긴다.
  이 artifact는 `windows-alpha-status.txt`, `KNOWN_ISSUES.md`, `status/*.status`를 포함해
  실패 class를 raw log만으로 추적하지 않게 한다.
  Q172 hosted run `https://github.com/deeyed/qstar/actions/runs/27508325529`은
  `src/executor.c`의 POSIX `<poll.h>` include에서 `make all CC=gcc`가 실패했다. Q179는
  `src/executor.c`와 `src/ninja.c`의 POSIX process runner boundary를 `_WIN32` compile stub으로
  분리하고, `qstar_platform_mkdir`/`qstar_platform_lstat`로 Windows C library signature 차이를
  줄인다. Q179 hosted run `https://github.com/deeyed/qstar/actions/runs/27527243941`은
  `make all`, `qstar --version`, native alpha smoke를 통과했고 Windows execution corpus에서
  멈췄다. Q218부터 실패한 `windows-native-alpha`와
  `windows-execution` script는 `native-alpha-detail/` 또는 `windows-execution-detail/`
  아래에 inner temp `.out`/`.err`, corpus `build/qstar`, response files, generated files,
  replay/action-log, Ninja files를 복사한다. 다음 Windows 구현 라운드는 Actions console log보다
  이 detail bundle을 먼저 본다. Q219는 `src/platform_process.c`를 추가해 Stella action,
  Stella/Ninja test artifact runner, QStar Ninja launcher가 같은 process start/wait/kill/status
  contract를 사용하게 했다. Windows 쪽은 argv vector to command-line quoting, env block, cwd,
  stdout/stderr pipe setup, timeout/terminate/status normalization을 이 layer 안에 모았다.
  Q220은 Stella action runner의 Windows backend를 `CreateProcessA`로 채운다. Windows Stella는
  compile/link/custom/run action을 실행하고, stdout/stderr capture, exit code propagation,
  timeout kill, run_target `expect.contains`, action-log/replay의 `windows_command_line` 기록을
  platform layer로 처리한다. QStar의 DSL-facing argv는 shell-free vector로 남고, MSYS2 alpha
  fixture의 `.sh` command만 effective command line에서 `sh <script>.sh ...`로 감싼다. 다음
  Windows 작업은 hosted alpha rerun 결과를 기준으로 Ninja parity, MSVC/clang-cl execution,
  sharedlib/artifact packaging을 좁힌다.
  Q164부터 `src/daemon.c`는 Windows stub을 제공해 Unix socket include 실패를 피하고,
  Windows host에서 `qstar daemon`/`--use-daemon=always`는 named pipe 구현 전까지 deferred
  diagnostic으로 처리한다.
  Q173은 Windows artifact implementation plan을 `docs/windows-artifact-policy.md`와
  `docs/windows-artifact-graph-ir.md`에 봉인한다. `.exe`는 explicit primary artifact,
  static `.lib`는 Unix `.a`와 분리된 explicit static archive, Windows sharedlib는 primary
  runtime `.dll` plus secondary import `.lib` multi-output target으로 설계한다.
  `qstar.target_file("//:plugin")`은 runtime `.dll`, future selector
  `qstar.target_file("//:plugin", { artifact = "import_lib" })`은 import `.lib`를 뜻한다.
  PDB/debug artifact는 opt-in/deferred이며 implicit install/stage 대상이 아니다.
  Q174부터 `tests/corpus/windows-artifacts`는 forward-looking 문서 fixture가 아니라
  executable/staticlib regression gate도 맡는다. `windows_fake` toolset은
  package-local fake `clang-cl`/`lib` 도구로 target-local `.exe`/static `.lib`와
  explicit `artifact_name` `.exe`/static `.lib`를 Stella와 Ninja 양쪽에서 build하고,
  stage/install layout은 `.exe`를 `bin/`, static `.lib`를 `lib/` 아래로 확인한다.
  QStar DSL path는 Windows에서도 `/`로 정규화된 package-relative path이며,
  backslash path와 drive-letter package path는 금지된다. Windows-like path 문자열이
  실제 compiler/linker option이면 `compile_options`/`link_options` argv item으로 두고
  `response_style = "msvc"`로 escape한다. `.exe`는 target-local `artifact_name`으로
  명시할 수 있고, 외부 system library는 MSVC-like target에서 `.lib`로 렌더링한다.
  explicit static `.lib`는 target-local `artifact_name`으로
  planning할 수 있다. Q165 prep gate는 `.exe` metadata, fake static `.lib` Stella/Ninja
  build, Windows sharedlib diagnostic parity를 같이 확인했고, Q174는 `.exe`/static `.lib`
  install/stage layout까지 더한다. Automatic `.lib`, `.dll`, import `.lib`, PDB/debug은
  native Windows 검증 전까지 official contract가 아니다. 상세 정책은
  `docs/windows-artifact-policy.md`에 둔다.
- `make qstar-medium-project-readiness-tests`는 Stella executor와 Ninja backend의 clean,
  no-op, incremental build 시간을 `medium_project_gate ...` line protocol로 기록한다.
  Round Q92 기준 timing threshold는 report-only가 기본이며,
  `QSTAR_MEDIUM_PERF_REPORT_ONLY=0`이면 hard gate로 승격된다.
- `make qstar-generic-dsl-backend-parity-tests`는 Q193 generic DSL backend seal이다.
  self-host, Stella generated/object/sharedlib smoke, Ninja backend parity, Linux
  validation, medium Stella/Ninja timing summary를 current `qstar.toolset`/`qstar.config`
  surface로 묶는다.
- `make qstar-large-project-performance-tests`는 200/500 target synthetic corpus의 scaling을
  report-only line protocol로 기록한다. `tools/perf-summary.sh`는 medium/large output을
  읽어 min/median/max와 Stella/Ninja ratio를 요약한다. `--format markdown` output은
  `Skipped reason`, `Warn threshold`, `Hard threshold` column을 포함하므로 macOS local run과
  Linux CI artifact가 같은 table format을 쓴다. `qstar-performance-release-gate`는
  medium/large를 `QSTAR_PERF_REPEAT` 횟수만큼 실행하고 `dist/perf/*-release-summary.md`를
  만든다.
  Warning threshold는 report-only 기준이고, hard threshold는 `--hard`와 함께 쓰면 실패 기준이다.
  Q166 repeat-3 local snapshot은 `docs/perf/q166-large-performance-refresh.md`에 보관하며,
  normal Stella가 200/500 target large corpus에서 Ninja median과 같은 급 또는 더 빠른
  수치를 보였다는 0.7 readiness 입력으로 쓴다.
  Round Q149 이후 medium/large gate는 socket이 허용되는 host에서 experimental daemon도
  `backend=stella-daemon`으로 측정한다. Daemon은 `clean`, `noop`, `incremental`을 모두
  기록하고, socket bind가 sandbox 정책으로 막히면
  `elapsed_ms=skipped reason=socket-bind-not-permitted`로 남긴다. Summary tool은 daemon과
  Ninja의 ratio도 같은 표에 포함한다.
  Linux CI artifact는 `dist/perf/linux-<compiler>-medium-perf.txt`,
  `dist/perf/linux-<compiler>-medium-summary.txt`,
  `dist/perf/linux-<compiler>-medium-summary.md` 이름을 사용한다.
  Q167부터 `daemon_socket_smoke=true` opt-in job은
  `make qstar-linux-daemon-validation-tests`를 실행하고
  `linux-daemon-validation-status.txt`, `linux-daemon-validation-reason.txt`,
  `linux-daemon-validation-*-trace.txt`에 `backend=inotify` watcher trace와 skip/fail reason을 남긴다.
- Persistent Stella daemon은 watcher-backed memory-state streaming beta opt-in 기능이다. 명령 namespace는
  `qstar daemon`이고, `qstar stella-daemon`은 채택하지 않는다. 현재 구현은
  `qstar daemon --socket path --start|--stop|--serve|--status`,
  `qstar build --use-daemon=auto|never|always --daemon-socket path`다. `auto`는 연결 실패 시
  normal Stella build로 fallback하고, `always`는 실패한다. Daemon은 같은 process에서
  Graph IR와 lowered plan cache 결과를 유지하고, `state.db`/`deps.db` memory snapshot을 먼저
  사용한다. 성공 build 뒤에는 disk DB를 계속 writeback하므로 daemon crash 후 일반 Stella build도
  복구 가능하다. macOS는 `kqueue`, Linux는 `inotify` watcher path를 사용한다. Watcher가 active이면
  authoring input stat scan을 매 request마다 먼저 반복하지 않고, authoring watcher event가 오면
  graph를 다시 load한다. Watcher가 unavailable이거나 incomplete이면 기존 fingerprint scan으로
  fallback한다. 일반 source/header 변경은 기존 Stella dirty-check가 처리한다.
  Build response는 `qstar-daemon-stream-v1` event stream이며 CLI는 frame을 숨기고 payload만
  렌더링한다. Event type은 `progress`, `diagnostic`, `action`, `summary`, `output`이고
  마지막은 `final <status-code>`다. Stream 시작 후 daemon이 죽으면 fallback하지 않고 실패한다.
  `--schedule-trace`에서는 `dirty_state_memory status=hit|miss|writeback`,
  `deps_memory status=hit|miss|writeback`, `daemon_watcher status=active|event|unavailable`로
  memory/watcher path를 확인한다. Background start/stop과 Windows named pipe는 deferred다. 정본 설계는 `docs/daemon/stella-daemon.md`,
  사용자 reference는 `wiki/reference/stella-daemon.md`에 둔다.
  Round Q150부터 `qstar daemon --socket path --query method` read API가 있으며 method는
  `hello`, `workspace.info`, `targets.list`, `diagnostics.list`,
  `compile_commands.path`, `build.summary`다. 계약 문서는
  `docs/contracts/daemon-read-api.md`에 둔다. Build/test/clean mutation은 read API에 없다.
  Round Q151 판단 이후 daemon은 `0.6.0-beta`에서 documented beta opt-in 기능으로 승격한다.
  기본 `qstar build`는 daemon을 쓰지 않고, `--use-daemon=auto|always`와
  `qstar daemon --query ...`는 명시적으로 선택할 때만 쓴다. Q153 이후 socket directory/file
  owner-only 검사, owner mismatch reject, protocol mismatch diagnostic, package root/build_dir
  hard reject는 beta opt-in 기준으로 구현되어 있다. Q154 이후 `qstar daemon --start/--stop`,
  pid file, lock file, duplicate start diagnostic도 beta opt-in 기준으로 구현되어 있다. Stable
  daemon API version promise, Linux daemon CI lane, Windows named pipe는 아직 default-on 전에
  닫아야 할 gap으로 남긴다. Q175부터 daemon beta lifecycle seal은 `--start`, `--status`,
  `--stop`, duplicate start, package-root/build-dir mismatch hard reject, stale socket cleanup,
  stale pid cleanup, stale lock cleanup을 regression으로 묶는다. Linux
  `daemon_socket_smoke=true` lane은 inotify trace와 함께 `qstar daemon --status` artifact도
  남긴다. Windows named pipe는 계속 deferred다. 판단 문서는
  `docs/daemon-beta-readiness.md`에 둔다.
- Stella dirty-check의 canonical fast state는 `build/qstar/state/state.db`다.
  `build/qstar/state/actions.json`은 기본 생성물이 아니며
  `QSTAR_DEBUG_STATE_DUMPS=1 qstar build ...`로 요청한 debug/export dump다.
  `build/qstar/state/graph.json`과 성공 `build/qstar/state/last-summary.json`은
  `QSTAR_DEBUG_STATE_DUMPS=1` 또는 `--schedule-trace`에서만 생성된다. 실패 summary와
  `last-failure` replay는 즉시 기록된다.
- external command는 `qstar.cli { ... }` argv-vector로만 표현한다.
- domain-specific package flow도 generic primitive로 표현한다.

## 3. 핵심 rule과 helper

Target/rule:

- `qstar.executable`
- `qstar.staticlib`
- `qstar.sharedlib`
- `qstar.test`
- `qstar.config`
- `qstar.custom_target`
- `qstar.run_target`
- `qstar.group`
- `qstar.configure_file`
- `qstar.stage`
- `qstar.target_family`

Command/path helper:

- `qstar.cli`
- `qstar.status`
- `qstar.input`
- `qstar.output`
- `qstar.target_file`
- `qstar.stage_file`
- `qstar.files`
- `qstar.subdir`
- `qstar.import_file`
- `qstar.import_module`
- `qstar.use_language`
- `qstar.join`
- `qstar.copy`
- `qstar.append`
- `qstar.merge`
- `qstar.extend`

Toolset/build policy:

- `qstar.toolset`
- `tools.archive`와 `tools.link`는 core role이며 `qstar.cli { ... }` argv-vector다.
- compiler role은 provider namespace table 아래에 둔다:
  `tools = { c = { compiler = qstar.cli {"cc"} }, archive = qstar.cli {"ar"}, link = qstar.cli {"cc"} }`.
- External provider toolset 문법도 provider namespace를 직접 받는 형태다:
  `tools = { archive = qstar.cli {"ar"}, link = qstar.cli {"cc"}, zig = zig.tools { compiler = qstar.cli {"zig"} } }`.
- `response_files`, `response_style`
- `path_tools`, `allow_absolute_tools`
- compile/link option은 `qstar.config`와 target-local `lang.*`, `link_options`,
  `link_inputs`로 조합한다.
- macOS frameworks are authored only as `link = { frameworks = {...} }` inside a
  macOS-specific branch.
- `qstar doctor`는 resolved tool role, 누락된 PATH/package tool, response policy,
  depfile behavior를 보고한다.

## 4. 언어별 option 위치

언어별 option은 target top-level에 두지 않는다. 항상 `lang.*` 아래에 둔다.

```lua
qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = {
      public_headers = {"include/core.h"},
      public_include_dirs = {"include"},
      compile_options = {"-Wall"},
    },
  },
}
```

언어 namespace:

- `lang.c`: C headers, include dirs, defines, compile options
- `lang.cxx`: C++ headers, include dirs, standard, modules skeleton, compile options
- `lang.asm`: assembler include dirs, compile options, preprocess flag
- `wiki/reference/language-providers.md`: built-in provider registry와 external object
  artifact bridge boundary
- `wiki/reference/language-providers.md`: dynamic language provider namespace와 GLP 문법

공통 option은 target top-level로 되돌리지 말고 `qstar.config`로 선언한다. Config label은
target의 `configs`에서 참조한다.

```lua
qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    cxx = { compiler = qstar.cli {"c++"} },
    asm = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
}

qstar.config "module_c" {
  toolset = "//:host",
  lang = {
    c = {
      public_include_dirs = {"include"},
      compile_options = {"-std=c23", "-Wall", "-Wextra"},
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:module_c"},
  sources = {"src/core.c"},
  lang = {
    c = {
      compile_options = {"-DCORE_BUILD=1"},
    },
  },
}
```

Merge rule:

- list field는 `configs` 순서대로 append된다.
- target local list field는 마지막에 append된다.
- scalar field는 뒤의 config가 앞의 config를 override하고 target local scalar가 최종 override한다.
- config는 source, deps, command, output을 만들지 않는다.
- config는 사용하는 target보다 먼저 평가되어야 한다. 보통 policy `.qst`를 먼저 import하고 leaf target fragment를 나중에 읽는다.

## 5. Generated artifact와 stage

Generated file이나 package 변환은 `qstar.custom_target`으로 표현한다.

```lua
qstar.custom_target "package_blob" {
  inputs = {qstar.target_file("//:app")},
  outputs = {
    qstar.output("generated/app.bin", {
      group = "packages",
    }),
  },
  command = qstar.cli {"tools/package-object", qstar.input(0), qstar.output(0)},
}
```

지원되지 않는 언어가 object file을 만들 수 있다면 object artifact bridge를 쓴다.

```lua
qstar.custom_target "foreign_object" {
  inputs = {"src/foreign_source.ext"},
  outputs = {qstar.output("generated/foreign.o", {format = "object"})},
  command = qstar.cli {"tools/compile-foreign.sh", qstar.input(0), qstar.output(0)},
}

qstar.executable "app" {
  sources = {"src/main.c", qstar.output("generated/foreign.o")},
}
```

이 pattern은 새 language provider가 아니라 artifact bridge다. External compiler는 package-local
wrapper나 toolset `path_tools`로 명시적으로 허용한다.

Staging은 install과 다르다. release bundle, test fixture bundle, package tree 같은
copy-only layout은 `qstar.stage`를 쓴다.

```lua
qstar.stage "bundle" {
  root = "stage/bundle",
  files = {
    qstar.stage_file(qstar.target_file("//:package_blob"), "share/app.bin"),
    qstar.stage_file("assets/config.txt", "config.txt"),
  },
}
```

Deps-only aggregate는 `qstar.group`으로 표현한다. Group은 command, output, artifact,
install 대상이 아니며 `qstar.target_file("//:group")`도 금지된다.

```lua
qstar.group "module_parts" {
  deps = {
    "//lib:core",
    "//plugins:main",
  },
}
```

## 6. Run smoke

External smoke wrapper는 `qstar.run_target`이다. QStar는 wrapper 내부 도구를
깊게 소유하지 않는다.

```lua
qstar.run_target "smoke" {
  deps = {"//:bundle"},
  command = qstar.cli {"tools/smoke.sh", qstar.target_file("//:package_blob")},
  timeout = 10,
  expect = {
    contains = "SMOKE_OK",
    file = "smoke.log",
  },
}
```

Failure class는 `expect-missing`, `timeout`, `exit-code`처럼 분리되고,
`qstar last-failure`와 `qstar replay <action-id>`가 재현 정보를 출력한다.

## 7. 안전한 Lua subset

허용:

- `local function`
- `local` 변수
- table literal
- `ipairs`, `pairs`
- `table.insert`
- safe `string.*`
- QStar builtin authoring helpers: `qstar.join`, `qstar.copy`, `qstar.append`, `qstar.merge`,
  `qstar.extend`

금지:

- global assignment
- `io.open`
- `os.execute`
- `require`
- `load`, `loadfile`, `dofile`
- `debug`, `package`
- process/network/time/random API

Helper module은 Lua `require`가 아니라 `qstar.import_module`로 읽는다.
문자열 `$VAR` 치환은 없으므로 path와 option은 Lua local 값으로 조립한다.

```lua
local common = qstar.import_module("qstar/modules/common")
qstar.import_file("qstar/policies/warnings.qst")
```

## 8. Agent가 먼저 읽을 파일

일반 authoring:

1. `README.md`
2. `getting-started.md`
3. `concepts/workspace-project-package.md`
4. `concepts/labels-and-fragments.md`
5. `concepts/targets-and-actions.md`
6. `concepts/language-namespaces.md`

Reference:

1. `reference/qstar-lua.md`
2. `reference/backends.md`
3. `reference/modules.md`
4. `reference/configs.md`
5. `reference/target-rules.md`
6. `reference/toolsets.md`
7. `reference/custom-target.md`
8. `reference/object-artifacts.md`
9. `reference/run-target.md`
10. `reference/performance-gates.md`
11. `reference/diagnostics.md`

Package/artifact flow:

1. `tutorials/package-artifact.md`
2. `cookbook/objcopy.md`
3. `cookbook/staging.md`
4. `cookbook/run-target-smoke.md`
5. `cookbook/response-files.md`

## 9. Useful CLI

```sh
qstar docs
qstar docs --path
qstar docs --ai-index
qstar docs --show reference/qstar-lua.md
qstar --version
qstar version
qstar help
qstar help build
qstar init app hello
qstar --file qstar.lua check //...
qstar --file qstar.lua lint //...
qstar --file qstar.lua fmt --check qstar.lua
qstar --file qstar.lua list-targets --format json
qstar --file qstar.lua query //:target
qstar --file qstar.lua doctor
qstar --file qstar.lua explain //:target
qstar --file qstar.lua dry-run //:target
qstar --file qstar.lua emit-ninja //:target
qstar --file qstar.lua build //:target --explain-cache
qstar --file qstar.lua build //:target --progress plain
qstar --file qstar.lua build //:target --verbose --progress plain
qstar --file qstar.lua build //:target --schedule-trace --progress plain
qstar --file qstar.lua build //:target --progress off --color never
qstar docs --show reference/progress-output.md
qstar --file qstar.lua -B out/qstar -G stella build //:target
qstar --file qstar.lua -G ninja build //:smoke
qstar --file qstar.lua test //...
qstar --file qstar.lua stage //:bundle --dry-run
qstar --file qstar.lua install //:target --prefix /tmp/qstar-install --dry-run
make qstar-linux-validation-tests
make qstar-windows-prep-tests
make qstar-windows-native-alpha-tests
qstar --file qstar.lua why-rebuild //:target
qstar --file qstar.lua clean --target //:target
qstar --file qstar.lua log //:target
qstar --file qstar.lua last-failure
qstar --file qstar.lua action-log //:target:compile:0
qstar --file qstar.lua replay //:target:action:0
qstar lsp --stdio
```

## 10. Removed surface

다음은 되살리면 안 된다.

- `qstar.toml`
- `qstar.workspace`
- legacy qs fragment suffix
- legacy API aliases
- top-level `include_dirs`, `public_headers`, `cflags`, `cxxflags`, `modules`
- `qstar.target_file("//:group")`
- domain-specific target/rule builtin
