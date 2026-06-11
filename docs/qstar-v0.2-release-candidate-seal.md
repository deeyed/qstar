# QStar v0.2 Release Candidate Seal

QStar v0.2 RC는 Cale frontend/backend와 분리된 standalone build system contract다.
이 문서는 v0.2 authoring surface가 release candidate로 올라갈 수 있는 범위와 아직
experimental로 남는 범위를 분리한다.

```txt
status: v0.2 release candidate
binary: qstar/build/bin/qstar
build: make -C qstar
check: make -C qstar check
release gate: make -C qstar qstar-v0.2-rc-tests
root Makefile integration: none
Cale build integration: deferred
frontend/backend internal API integration: none
```

QStar는 compiler가 아니다. C, C++, Cale, assembly source를 build input으로 보고,
toolchain/profile에 맞는 process invocation과 artifact graph를 만든다. Header와
generated header는 path/dependency/install surface이며, QStar가 C/HCL 내용을 해석하지
않는다.

## Release Candidate Surface

다음 항목은 v0.2 RC에서 유지할 authoring/command contract다.

- Root/fragment naming: `qstar.lua`, optional `qstar.project { root = "." }`,
  `<folder>.qst`; `qstar.workspace` is removed.
- Target rules: `qstar.executable`, `qstar.staticlib`, `qstar.test`,
  `qstar.custom_target`, `qstar.run_target`, `qstar.configure_file`, `qstar.stage`.
- Lint grouping: `qstar.target_family` for multi-arch shared-source policy.
- Helpers: `qstar.cli`, `qstar.input`, `qstar.output`, `qstar.target_file`,
  `qstar.files`, `qstar.subdir`, `qstar.select`, `qstar.join`.
- Language namespaces: `lang.c`, `lang.cxx`, `lang.asm`, `lang.cale`.
- Toolchain/profile input: `qstar.profile`, `--profile`, compiler path override,
  sysroot/resource-dir/include/linker settings.
- Executor: compile, archive, link, generated action, config header, test, install,
  stage, run target, incremental cache, depfile input tracking.
- UX: `lint`, `fmt`, `list-targets --format json`, `query`, `doctor`, `explain`,
  `dry-run`, `build`, `test`, `install`, `stage`, `log`, `last-failure`, `replay`,
  `action-log`, `clean`.
- Editor surface: VSCode package metadata, snippets, syntax highlighting, LSP
  diagnostics, hover, completion, definition/references, target tree/query commands.
- Diagnostics: text diagnostics, `--diagnostics json`, `qstar-lint-v1`,
  `qstar-action-diagnostic-v1`, action replay, stable failure kind separation.

The old v0.1 aliases are not RC surface. `qstar.exe`, `qstar.genrule`,
`qstar.config_header`, `qstar.write_config_header`, top-level `include_dirs`,
top-level `public_headers`, top-level `private_headers`, top-level `modules`,
top-level `cflags`, top-level `cxxflags`, and top-level `cxx_standard` remain
removed APIs with stable diagnostics.

## Release Gate

The RC gate is QStar-local:

```txt
make -C qstar qstar-v0.2-rc-tests
make -C qstar qstar-release-candidate-tests
make -C qstar qstar-full-regression-tests
make -C qstar qstar-systems-corpus-tests
make -C qstar vscode-extension-tests
```

All these targets currently resolve to the QStar local `check` harness. The
separate names are CI/automation anchors. The aggregate check covers:

- standalone binary build
- Lua evaluator and sandbox policy
- graph/query/explain/dry-run/check/lint/fmt
- LSP diagnostics, hover, completion, definition/reference, symbols
- VSCode package drift and no-generated-artifact policy
- C, C++, Cale-by-process, and assembler source planning/build smoke
- generated source/header chaining, config header, binary/blob artifact flow
- response file rendering, long command execution, parallel executor, replay UX
- workspace/package root/visibility/include propagation checks
- install/stage/test/run target smoke
- systems firmware corpus with freestanding C/ASM, linker script, image transform,
  staged package layout, PE/COFF naming, and external smoke wrapper

## Stable Failure Contract

v0.2 RC keeps failure diagnosis as part of the developer contract:

- `link-failure`: final link command failed.
- `objcopy-failure`: artifact transform command failed.
- `package-failure`: copy-only stage/package command failed.
- `qemu-timeout`: emulator-style run target timed out.
- `marker-missing`, `exit-code`, `timeout`: generic run target failure classes.

These failure kinds appear in stdout action diagnostics, stderr JSON diagnostics
when requested, `build/qstar/logs/last-failure.replay`, and `qstar replay`.

## Experimental Or Deferred

The following surfaces are intentionally not RC-stable:

- `cale build` integration and root `Makefile` ownership.
- Cale frontend/backend internal API integration.
- Remote package fetch, registry, lockfile resolution, and remote cache protocol.
- Ninja or other generator backends.
- Full shared library executor/install metadata.
- HCL parsing/import/export semantics.
- Full public machine-readable graph/cache schema beyond the documented diagnostic
  skeletons.
- Automatic board/platform-specific target kinds. QStar represents those flows
  through generic executable/staticlib/custom/run/stage rules and profile policy.

## Readiness Verdict

QStar v0.2 RC is suitable for a standalone pilot in a local systems project when
the project can express its flow through generic QStar rules, profile-driven
toolchain policy, and package-local generated/staged artifacts. It is not yet a
replacement for mature package managers or multi-repo dependency resolvers.
