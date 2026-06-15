# QStar v0.1 Hardening Seal

> Current note: Round 47의 현재 authoring 정본은
> `docs/qstar-v0.2-authoring-spec.md`다. 이 문서는 Round 38 v0.1 contract의
> 역사 기록으로 유지한다.

QStar v0.1은 compiler frontend/backend와 분리된 standalone developer build system으로
봉인한다. 이 상태의 목적은 `downstream build` 통합 전에 QStar만으로 small-to-medium local
C/C++/external-language-by-process 프로젝트를 작성, 설명, 빌드, 테스트, 설치, 재빌드 추적할 수
있음을 고정하는 것이다.

```txt
status: v0.1 standalone developer build system
binary: qstar/build/bin/qstar
build: make -C qstar
check: make -C qstar check
install: make -C qstar install PREFIX=/path
release gate: make -C qstar qstar-v0.1-release-tests
root Makefile integration: none
downstream build integration: deferred
frontend/backend internal API integration: none
```

QStar는 compiler가 아니다. C, C++, external source를 build input으로 보고 target/profile에
맞는 process invocation과 artifact graph를 만든다. `.h`, `.hpp`, generated header,
future `.h`은 path, dependency, install/export surface일 뿐이며 QStar가 header
syntax를 해석하지 않는다.

## Compatibility Contract

v0.1에서 당분간 유지할 authoring/command surface는 다음이다. 이 목록은 source 작성자가
QStar를 독립 빌드시스템으로 시험할 수 있게 하는 최소 계약이다.

- Historical authoring files: `qstar.lua`, `qstar.workspace`, subdir `.qst`
  fragment. Current v0.2 hard cut removes `qstar.workspace`; see
  `docs/qstar-v0.2-authoring-spec.md`.
- Target rules: `qstar.executable`, `qstar.staticlib`, `qstar.test`, `qstar.custom_target`,
  `qstar.run_target`, `qstar.configure_file`, `qstar.output`, `qstar.cli`.
- Plan-only or gated rules: `qstar.sharedlib` local build/install은 stable unsupported.
- Helpers: `qstar.modules`, `qstar.files`, `qstar.join`, `qstar.subdir`.
- Labels: `:local`, `//:name`, `//path:name`, `@pkg//path:name`.
- Dependency fields: `deps`, `public_deps`, `private_deps`, `visibility`.
- Source/header fields: `sources`, `lang`; `public_headers`/`private_headers`는
  `lang.c`, `lang.cxx`, `lang.cxx` 아래에서만 authoring surface다.
- Toolchain/profile fields: `toolchain`, `stdlib`, `libs`, `lib_dirs`, `link.frameworks`.
- Commands: `list-targets`, `query`, `doctor`, `check`, `explain`, `dry-run`,
  `build`, `test`, `install`, `why-rebuild`, `log`, `last-failure`,
  `action-log <action-id>`, `replay <action-id>`, `clean`, `init`,
  `--dump-graph`.
- Diagnostics: default text and `--diagnostics json` skeleton.

Internal text dump spacing, action hash values, `build/qstar/state/actions.json`, and
`build/qstar/state/graph.json` are diagnostic/cache implementation details. They are
regression-tested for QStar itself but are not yet a public remote-cache protocol.

## Integrated Smoke Surface

The v0.1 release gate ties these features together:

- Graph evaluation, closure, command plan, and deterministic explain output.
- Source discovery for C, C++, external-language-by-process source kinds.
- Toolchain/profile schema v2: host/clang/external-tool profile rendering, sysroot,
  resource dir, include dirs, lib dirs, response file policy.
- Local executor: compile, archive, link, generated action, config header, test,
  install, failure replay.
- Incremental state: `build/qstar/state/actions.json`, `build/qstar/state/graph.json`,
  `build/qstar/state/last-summary.json`, depfile-discovered input tracking, cache miss
  reasons.
- `compile_commands.json` generation.
- Response files for long commands with POSIX/Windows/MSVC-style policy markers.
- Optional process-level parallel compile executor with failure, timeout, and
  cancel event stream.
- Package/workspace ownership, visibility skeleton, public/private include
  propagation, package-local output gate.

## Release Gate

Run the v0.1 seal from the QStar directory boundary:

```txt
make -C qstar qstar-v0.1-release-tests
make -C qstar qstar-v0.1-hardening-tests
make -C qstar qstar-standalone-integration-tests
qstar action-log <action-id>
qstar replay <action-id>
```

All three targets intentionally resolve to QStar's local `check` harness today.
The separate names are compatibility anchors for future CI selection.

The release gate covers the full repository-local sample corpus:

```txt
qstar/tests/manual/c-only
qstar/tests/manual/generated
qstar/tests/manual/generated-extra
qstar/tests/projects/c-app-lib-test
qstar/tests/projects/cxx-mixed
qstar/tests/projects/generated-config
qstar/tests/projects/multipkg
qstar/tests/projects/systems-firmware
```

Manual samples are for direct user experimentation. `tests/projects` is the
release corpus and must stay self-contained inside the QStar tree.
`systems-firmware` is the central systems-grade corpus: it combines
AArch64 freestanding C/ASM, linker script policy, raw image transform, ESP/RPi
staging, UEFI PE/COFF profile output, and QEMU wrapper smoke without adding
board-specific QStar keywords.

Round 59 release hardening adds failure diagnostics over this corpus. Link
failure, objcopy/image transform failure, package/stage failure, and run timeout
must be visible as separate `failure_kind` values in stdout JSON diagnostics,
stderr diagnostics, `qstar last-failure`, and `qstar replay`. The intent is that
users can identify the failing stage without reverse-engineering a
generic command failure.

## Standalone Use Before external language Build

QStar v0.1 is usable before `downstream build` exists because it owns its own binary,
profile reader, executor state, logs, install manifest, and sample corpus. A
typical flow is:

```txt
make -C qstar
qstar/build/bin/qstar init c-app /tmp/my-qstar-app
cd /tmp/my-qstar-app
/path/to/qstar/build/bin/qstar --file qstar.lua build //:app
/path/to/qstar/build/bin/qstar --file qstar.lua test //:unit
/path/to/qstar/build/bin/qstar --file qstar.lua install //:app --prefix /tmp/qstar-install --dry-run
```

For external sources, QStar still uses public-ish process invocation only. It does
not link against compiler frontend/backend internals.

## Deferred After v0.1

- Root `Makefile` or `downstream build` integration.
- Remote package fetch, lockfile resolution, registry, and remote cache.
- Ninja generator.
- Full shared library executor and platform install metadata.
- header-language parsing/import/export.
- external compiler internal API integration.
- Stable machine-readable graph/cache protocol.
