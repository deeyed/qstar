# QStar Specification

이 폴더는 QStar의 상세 사양을 둔다. QStar는 독립 build graph manager이며 특정
downstream 언어나 project에 종속되지 않는다. 언어별 의미 해석은 compiler와 language
provider가 맡고, QStar는 source/header/output path, target graph, command plan,
build/test/install/stage 실행 정책만 관리한다. HCL 파일도 QStar 관점에서는 header-like
file path일 뿐이며, 내용 해석은 해당 language provider가 맡는다.

## Core Rule

QStar는 build graph만 담당한다.

QStar 파일에는 다음을 넣을 수 있다.

- target graph
- package-local source/header/output path
- `qstar.profile` toolchain/profile declaration
- profile-selected target/link/external-tool policy

QStar 파일에는 다음을 넣지 않는다.

- secure profile이나 UB category override
- audit profile
- compiler safety policy
- dependency version resolution
- package lock data
- registry publish/fetch policy

QStar는 별도 mandatory profile config를 읽지 않는다. 설정 진입점은 `qstar.lua` 하나이며,
dependency resolver나 lock data는 QStar 밖의 package manager가 맡는다.

## File Roles

| File or directory | 역할 |
| --- | --- |
| `qstar.lua` | package root의 QStar orchestration file, `qstar.project`, `qstar.profile`, `qstar.config` metadata |
| `<dirname>.qst` | 큰 프로젝트의 subdir QStar fragment |
| `<module>/<module>.qsm` | side-effect 없는 QStar helper module, `return table` 전용 |
| `src/` | implementation source root |
| `include/` | public header/install surface root |

작은 프로젝트는 root `qstar.lua` 하나로 충분하다. 큰 프로젝트는 root `qstar.lua`가 `qstar.subdir(...)`로 subdir fragment를 불러오고, subdir에는 `<dirname>.qst`를 권장한다.

## Documents

- `syntax.md`: Calua/QStar subset과 `qstar.*` API 문법.
- `model.md`: workspace, package, target, module, source/header file 모델.
- `graph-ir.md`: Internal canonical Graph IR와 deterministic text dump 정책.
- `pipeline.md`: explain-first v0 build pipeline과 future executor 방향.
- `qstar-v0-seal.md`: v0 authoring compatibility contract.
- `qstar-v0.1-hardening-seal.md`: v0.1 standalone hardening/release gate contract.
- `qstar-v0.2-authoring-spec.md`: v0.2 hard-cut authoring surface와 `lang.*` 정책.
- `qstar-v0.2-release-candidate-seal.md`: v0.2 RC stable/experimental surface와 release gate.
- `qstar-v0.3-seal.md`: v0.3 standalone release-candidate surface와 editor/corpus seal.
- `qstar-v0.4-stella-seal.md`: Stella 기본 backend, Ninja backend 후보, install/editor packaging seal.
- `qstar-self-host.md`: Makefile 유지와 QStar self-host graph/release gate 후보 계약.
- `ninja-backend-parity.md`: Ninja lowering parity, sharedlib policy, action-log/replay 계약.
- `language-provider-backend-contract.md`: Cale/Stella/Ninja/HCL language provider 경계 계약.
- `performance-gates.md`: Stella/Ninja clean, no-op, incremental timing gate 계약.
- `daemon-beta-readiness.md`: Stella daemon을 beta opt-in으로 올릴지 판단하는 Q151 gate.
- `daemon/stella-daemon.md`: Experimental persistent Stella daemon command namespace, Unix socket MVP, fallback, watcher, security, IDE/AI integration design.
- `contracts/daemon-read-api.md`: Stella IDE/AI가 사용할 daemon read-only query API 계약.
- `perf/stella-ninja-profile.md`: Ninja architecture profiling과 Stella plan-cache 성능 개선 방향.
- `perf/stella-plan-cache-design.md`: Stella lowered plan cache fingerprint, invalidation, internal file policy.
- `progress-output.md`: CMake-style progress output, warning/error color, `qstar.status` descriptions.
- `linux-validation.md`: Linux host validation path, Ubuntu gcc/clang CI workflow, install smoke, linux-x86_64 candidate tarball dry-run, Stella/Ninja medium perf artifact 조건.
- `windows-path-process.md`: Windows path/process/response-file pre-port contract와 manual native validation candidate workflow.
- `public-beta-release.md`: public beta packaging, install smoke, codesign, wiki sync gate.
- `releases/TEMPLATE.md`: GitHub prerelease notes template.
- `releases/v0.5.2-beta.1.md`: Stella daemon beta opt-in candidate release note.
- `qstar-v0.5-readiness.md`: 0.5 beta line으로 올릴지 판단하는 readiness gate.
- `qstar-pilot-readiness-seal.md`: formatter/help/wiki/CLI sync까지 묶은 pilot-readiness gate.
- `qstar-submodule-extraction-prep.md`: 독립 repo/submodule 전환 직전 체크리스트.
- `../wiki/`: 사용자/AI가 QStar project를 직접 작성할 수 있는 한국어 wiki.
- `../wiki/AI_INDEX.md`: Codex 같은 AI agent가 먼저 읽는 압축 색인.
- `../man/`: installed `man qstar`, `man qstar-lua` entrypoint.
- `network-policy.md`: Fetch-only network policy와 offline/frozen/locked mode.
- `examples.md`: 작은 앱, header facade, monorepo, generated source, platform split 예시.
- Package resolver, dependency source model, lock data는 QStar core 밖의 package
  manager가 맡는다. QStar standalone repo는 `qstar.lua`로 선언한 build graph와
  profile/toolchain surface만 소유한다.

## Canonical Status

QStar의 상세 문법과 pipeline 사양은 이 `docs/` 폴더를 canonical 위치로 본다.

Round 47부터 authoring syntax의 정본은 QStar v0.2다. Target top-level C/C++/Cale
language option은 제거됐고, header/include/compile/module option은 반드시
`lang.c`, `lang.cxx`, `lang.asm`, `lang.cale` 아래에 둔다. `qstar.exe`,
`qstar.genrule`, `qstar.config_header`,
`qstar.write_config_header`는 compatibility alias가 아니라 stable diagnostic을 내는
removed API다.

Round 48부터 generic command model은 `qstar.cli { ... }` argv-vector를 사용한다.
`qstar.custom_target`은 `command = qstar.cli { ... }`로 generated action을 만들고,
`qstar.run_target`은 build artifact 이후 external smoke command를 실행한다.

Round 55부터 boot/package staging은 `qstar.stage`가 담당한다. `qstar stage`는
install prefix와 별개로 ESP/RPi/firmware layout 같은 copy-only package tree를 만들고,
`build/qstar/stage/<label>/manifest.json`에 staged manifest와 dry-run diff를 남긴다.
Round 69부터 staged manifest는 v2 schema로 source kind와 producer를 기록하며,
`qstar.target_family`가 multi-arch shared source lint noise를 family 단위로 제어한다.

Round 60부터 v0.2 RC contract는 `qstar-v0.2-release-candidate-seal.md`에 둔다.
이 문서는 release-candidate surface와 experimental/deferred surface를 분리하고,
QStar-local full regression gate 이름을 고정한다.

Round 65부터 v0.3 RC contract는 `qstar-v0.3-seal.md`에 둔다. v0.3은 `.qst`,
`qstar.project`, `qstar.config`, `lang.*`, generic `qstar.cli`, systems firmware corpus,
stage/package/run target, VSCode/LSP/lint/formatter를 standalone build-system
surface로 봉인한다. Runtime version은 `qstar --version`과 `QSTAR_VERSION`이 같은
`0.3.0`이어야 한다.

Round 90의 v0.4 release/install contract는 `qstar-v0.4-stella-seal.md`에 둔다.
v0.4는 Stella를 기본 generator로 고정하고, `-G stella|ninja|auto`, `-B`,
`qstar.group`, `qstar.config`, `qstar.import_file`, `.qsm` module,
`qstar.project.generated_dir`, compact progress, Ninja comparison backend, medium
project performance gate를 함께 봉인한다. Runtime version은 `qstar --version`과
`QSTAR_VERSION`이 같은 `0.4.0-beta.1`, VSCode extension version은 `0.3.0`이어야 한다.

Round 100부터 0.5 readiness 판단은 `qstar-v0.5-readiness.md`에 둔다. 이 문서는
self-host, Stella/Ninja benchmark, Ninja parity, Linux/Windows status, docs/CLI drift,
medium project readiness, version policy, deferred surface를 한 번에 요약한다.
Round 110부터 runtime은 `0.5.0-beta.1` release-prep line으로 이동한다. Release note는
`releases/v0.5.0-beta.1.md`에 두고, VSCode extension package version은 runtime과 별도인
`0.3.0`으로 유지한다.

Round 117부터 runtime patch line은 `0.5.1-beta.1`로 이동한다. Release note는
`releases/v0.5.1-beta.1.md`에 두며, 이 patch line은 Stella/Ninja timing, Linux/Windows
readiness, sharedlib policy, Cale backend contract, install/package smoke를 묶어 다음
public beta 판단을 고정한다.

Round 116부터 Cale source는 Stella-only language-provider action으로 봉인한다. Ninja wrapper
lowering은 Cale provider의 argv/depfile/response-file/replay 계약이 별도 라운드로 정리될
때까지 deferred이며, HCL은 QStar가 해석하지 않는 header-like path다.

Round 101부터 progress output 계약은 `progress-output.md`에 둔다. QStar 0.5 UI line은
일반 build output에서 legacy scheduler stage wording, scheduler state, action id를 숨기고
`[ 75%] Linking CXX executable app` 같은 CMake-style action description을 출력한다.

## Round 15 구현 상태

QStar Round 14/15 기준으로 QStar는 독립 build-system binary다. 빌드는
`make -C qstar`로 수행하고, 기본 binary path는 `qstar/build/bin/qstar`다. 루트
Cale `Makefile`은 더 이상 QStar build/test/install target을 직접 소유하지 않는다.
또한 QStar는 아직 `cale build`에 연결되어 있지 않다.

Evaluator는 공식 Lua repository를 `vendor/lua` git submodule로 사용하며,
tag `v5.4.8`에 고정한다. Lua license 전문은 `LICENSE/lua.txt`에 보존한다.

현재 지원 command는 다음과 같다.

```txt
qstar --file qstar.lua --dump-graph
qstar --file qstar.lua list-targets
qstar --file qstar.lua query //:target
qstar --file qstar.lua doctor
qstar --file qstar.lua check //:target
qstar --file qstar.lua explain //:target
qstar --file qstar.lua dry-run //:target
qstar --file qstar.lua build //:target
qstar --file qstar.lua build //:target --explain-cache
qstar --file qstar.lua why-rebuild //:target
qstar --file qstar.lua log //:target
qstar --file qstar.lua last-failure
qstar --file qstar.lua clean --target //:target
qstar --file qstar.lua clean
qstar --file qstar.lua --diagnostics json check //:target
qstar --file qstar.lua --package-alias @core=/path/to/core explain //:target
qstar --file qstar.lua --profile debug --target arm64-apple-macos explain //:target
```

`--dump-graph`는 diagnostic용 raw Graph IR을 출력한다. `explain`은 target closure
검증, dependency-first ordering, action key 재료가 들어간 Build Plan IR 출력을
수행한다. 아직 compile, archive, link, package fetch, cache, Ninja action을 실행하지
않는다. `dry-run`은 같은 closure를 검증한 뒤 executor 모양의 step record를 출력하지만,
실제 action은 실행하지 않는다. `check`는 실제 작성 상태를 검증한다. package-root
source file, public/private header, generated action input은 존재해야 하고, generated
output은 정확히 하나의 `qstar.custom_target`이 만들 때만 아직 없어도 된다.

Round 3은 package alias map과 profile input의 뼈대를 추가했다. Alias map은 CLI fixture
input으로 주어지며 package resolver가 아니다. `--package-alias @core=/path/to/core`가
있으면 `@core//:core` 같은 external label을 resolved external dependency로 보고할 수
있지만, QStar가 그 package graph를 직접 읽지는 않는다.

Round 4는 header-file graph policy validation을 추가했다. Public header는 `include/`
아래 package-relative path여야 하고, private header도 package-relative path여야 한다.
Round 5부터는 QStar가 HCL을 해석한다는 암시를 제거했다. `qstar explain`은 header file을
opaque build-system entry로만 보고한다.

Round 5는 action key skeleton line도 추가했다. 이 line은 future cache key, Ninja
generation, internal executor에 필요한 deterministic material을 보여주지만 action을
실행하지는 않는다.

Round 6은 source discovery skeleton을 추가했다. 명시된 `sources` entry는
package-relative path인지 검증하고, suffix에 따라 C, Cale, assembler, preprocessed
assembler로 분류한다. `qstar explain`은 `source_file`과 `command_skeleton` line을
출력하지만 directory scan, glob expansion, tool 실행, object 생성은 하지 않는다.

Round 50은 `.s`/`.S` source를 local executor에 연결했다. QStar는 별도 standalone
assembler profile을 소유하지 않고 host/clang compiler driver에 `-x assembler` 또는
`-x assembler-with-cpp`를 넘겨 object를 만든다. `lang.asm.include_dirs`,
`lang.asm.compile_options`, `lang.asm.preprocess`는 asm action에만 적용된다.

Round 7은 generated output edge skeleton을 추가했다. 현재 정본 surface에서는
`qstar.custom_target`이 `inputs`, `outputs`, `command = qstar.cli { ... }`를 기록하고,
`qstar.output(path)`는 path spelling helper로 받아들인다. Generated source/header path는
effective `qstar.project.generated_dir` 아래 있어야 하며, 기본값은 `generated`다. 그
아래 generated source path는 정확히 하나의 generated action이 만들어야 한다. `qstar explain`은
`generated_edge`, `generated_action`, resolver-skeleton record를 출력하고, local
executor는 package-local generated action을 실행할 수 있다.

Round 8은 dry-run executor skeleton과 QStar-local sample project
`qstar/tests/manual/hello`를 추가했다. 이 sample은 root `qstar.lua`, subdir
`src/foo/foo.qst`, C source, public header, generated-source edge를 가진다. Cale
frontend/backend code를 건드리지 않고 `qstar --dump-graph`, `qstar explain`,
`qstar dry-run`을 시도하기 위한 첫 hand-authored fixture다.

Round 9는 authoring check를 추가했다. `qstar check`는 선언된 source/header/input file이
실제로 disk에 있어야 한다고 요구하는 첫 command다. 이 덕분에 `explain`은 초기 graph
sketch에 계속 유용하고, 실제 project에는 “나중에 build할 수 있을 만큼 coherent한가”를
더 엄격히 확인할 수 있다.

Round 10은 diagnostic origin과 query UX를 추가했다. Target declaration과 generated
action은 Lua source file/line을 보존하고, validation diagnostic은 가능한 경우
`field`/`label` metadata를 붙인다. Round 15의 `--diagnostics json`은 machine-readable
diagnostic skeleton을 출력한다. `list-targets`, `query`, `doctor`는 지금은 human-facing
command이고, 나중에는 LSP/query-server source가 될 수 있다.

Round 11은 실제 source selection skeleton을 추가했다. `qstar.files`는 package-root glob
pattern을 deterministic sorting과 `exclude` filtering으로 확장한다. 중복 source entry는
diagnostic으로 막고, `qstar.select`는 CLI/profile target input에서 실제 branch를 고른다.
아직 directory scanner나 일반 executor는 아니다.

Round 66은 read-only external profile input을 제거하고 `qstar.profile` DSL을 profile
authoring surface로 고정했다. `host`, `clang`, `cale` toolchain profile은
deterministic `build/qstar/out` path를 가진 real `command_argv` record를 만든다.

Round 13은 제한된 local executor로서 `qstar build`를 추가했다. Package-local generated
tool 실행, C source compile, static library archive, executable link를 수행할 수 있다.
stdout/stderr/action log는 `build/qstar/logs`에 저장하고 artifact는 `build/qstar/out` 아래에 쓴다.
Full Cale source build, assembly, remote package, cache, Ninja generation, 일반 process
execution은 아직 범위 밖이다.

Round 14/15는 incremental state와 diagnostic UX를 추가했다. Q121 이후 Stella는
`build/qstar/state/state.db` compact state를 먼저 읽고, `state.db`가 dirty-check의
canonical fast path다. Q141 이후 `build/qstar/state/actions.json`은
`QSTAR_DEBUG_STATE_DUMPS=1`에서만 쓰고, `build/qstar/state/graph.json`과 성공
`build/qstar/state/last-summary.json`은 `QSTAR_DEBUG_STATE_DUMPS=1` 또는
`--schedule-trace`에서만 쓰는 debug/export dump다. 실패 summary와 `last-failure` replay는
즉시 기록한다. action key와 output이 그대로이면
action을 건너뛴다. Q123 이후 Stella는 `build/qstar/state/deps.db` compact dependency
state를 함께 사용해 compiler depfile-discovered header list를 재사용한다.
또한 `compile_commands.json`, `why-rebuild`, `clean`, `log`, `last-failure`,
`--diagnostics json`을 제공한다. Action key v1에는 argv, input path metadata/content
hash, output path, selected profile/toolchain, 작은 environment whitelist가 들어간다.
이 executor는 아직 local developer executor이며, distributed cache나 Ninja-compatible
protocol은 아니다.

## Authoring Preview

작은 `qstar.lua`와 subdir `<dirname>.qst`를 직접 써보는 시점은 지금부터 가능하다.
Round 14/15에서는 `qstar list-targets`, `qstar query`, `qstar doctor`, `qstar check`,
`qstar explain`, `qstar dry-run`으로 graph shape, package-root file existence,
generated output edge, source classification, glob expansion, profile select,
dependency order, Build Plan IR/action key, command skeleton, dry-run step ordering이
기대대로 나오는지 볼 수 있다. C-only local fixture는 `qstar build`로 실제
compile/archive/link를 실행하고, 두 번째 빌드부터 cache-hit skip과
`compile_commands.json`까지 확인할 수 있다. Full Cale compiler integration, remote
package graph loading, distributed cache, Ninja generation은 아직 고정된 authoring
contract가 아니다.
