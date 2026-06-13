# QStar v0.3 Seal

QStar v0.3은 Cale frontend/backend와 독립적으로 동작하는 standalone build
system release candidate다. 이 문서는 v0.2 hard-cut authoring surface 위에
systems/firmware-style project corpus, editor UX, replay/diagnostic UX를 더해
`v0.3`으로 봉인할 수 있는 범위를 고정한다.

```txt
status: v0.3 release candidate
runtime version: qstar 0.3.0
extension package: qstar-vscode 0.2.0
release gate: make -C qstar qstar-v0.3-rc-tests
```

`qstar --version`과 `qstar version`은 `QSTAR_VERSION` authoring constant와 같은
값을 출력해야 한다. v0.3에서는 이 값이 `0.3.0`이다.

## Stable Surface

다음 항목은 v0.3 release candidate의 stable surface다. 이후 라운드에서 내부 구현은
바뀔 수 있지만, authoring surface는 compatibility note 없이 깨지지 않아야 한다.

- Root file은 `qstar.lua`다. `qstar.workspace`는 제거됐다.
- Subdir fragment suffix는 `.qst`다. `.qs`는 stable diagnostic 대상이다.
- Root metadata는 `qstar.project { name, version, root = "." }`다.
- Target/rule API는 `qstar.executable`, `qstar.staticlib`, `qstar.sharedlib`,
  `qstar.test`, `qstar.custom_target`, `qstar.run_target`,
  `qstar.configure_file`, `qstar.stage`다.
- Lint grouping API는 `qstar.target_family`다. Multi-arch target family의 intentional
  shared source warning만 scope-locally 제어한다.
- Generated/external command는 shell string이 아니라 `qstar.cli { ... }`
  argv-vector로 표현한다.
- `qstar.input`, `qstar.output`, `qstar.target_file` placeholder가 command plan과
  action key에 반영된다.
- Header/include/compile/module option은 target top-level이 아니라
  `lang.c`, `lang.cxx`, `lang.asm`, `lang.cale` 아래에만 둔다.
- `sources`, `deps`, `visibility`, `libs`, `frameworks`, `link_options`,
  `linker_script`, `defsyms`, `toolchain`, `stdlib`, `artifact_name`은
  language-agnostic target field로 유지한다.
- C, C++, assembler, Cale source는 process invocation 기반 build input으로 다룬다.
  QStar는 Cale compiler 내부 API나 HCL semantic checker에 연결되지 않는다.
- Incremental cache, `build/qstar/state/actions.json`, graph snapshot,
  `compile_commands.json`, action log, `last-failure`, `action-log`, `replay`,
  cache miss reason은 developer UX surface로 유지한다.
- `qstar lint`, `qstar fmt`, `qstar lsp --stdio`, VSCode extension, target tree query,
  hover/completion/definition/reference/document-symbol UX는 read-only graph/editor
  UX surface로 유지한다.
- Response file, parallel executor, timeout/cancel/failure propagation, staged package,
  run target smoke, external tool policy, artifact transform metadata는 local executor
  surface로 유지한다.

## Experimental Surface

The experimental surface is intentionally separated from the stable surface so
v0.3 can be used as a standalone build-system release candidate without
overclaiming package-manager or compiler-integration readiness.

다음 항목은 v0.3에서도 experimental 또는 deferred surface다.

- `cale build` 내부 통합.
- Cale frontend/backend 내부 API 직접 호출.
- Remote package fetch, registry, lockfile resolver.
- Ninja/CMake generator backend.
- Full shared library executor and platform loader policy.
- C++ modules build pipeline. `lang.cxx.modules.enabled = true`는 stable diagnostic이다.
- HCL declaration import/export semantic checking. QStar는 HCL을 header path로만 본다.
- Rich provider plugin ABI for non-C/C++/Cale languages.
- Cross-machine remote execution and content-addressed remote cache.

## Corpus Gate

v0.3 release gate는 QStar-local corpus가 모두 새 surface로 유지되는지 확인한다.

- C app/lib/test corpus: `qstar/tests/projects/c-app-lib-test`
- C++ mixed corpus: `qstar/tests/projects/cxx-mixed`
- Generated config corpus: `qstar/tests/projects/generated-config`
- Local multi-package corpus: `qstar/tests/projects/multipkg`
- Source-dir style corpus: `qstar/tests/projects/source-dir-style`
- Systems firmware corpus: `qstar/tests/projects/systems-firmware`
- Stage/package smoke: `qstar.stage`, dry-run diff, staged manifest
- Run target smoke: timeout, marker, stdout/stderr log, replay
- VSCode/LSP/lint/formatter/package corpus:
  `qstar/editors/vscode/qstar`

Recommended release gate:

```txt
make -C qstar check
make -C qstar qstar-v0.3-rc-tests
make -C qstar qstar-project-corpus-tests
make -C qstar qstar-systems-corpus-tests
make -C qstar vscode-extension-tests
```

`qstar-v0.3-rc-tests` is intentionally an alias of the QStar-local `check`
harness. `vscode-extension-tests` is now a narrower package metadata and payload
smoke for the VSCode extension. These names exist so release scripts and editor
packaging can depend on stable target names.

## Removed Surface Lockdown

The following removed surface must stay rejected:

- `qstar.exe`: use `qstar.executable`
- `qstar.genrule`: use `qstar.custom_target`
- `qstar.config_header` and `qstar.write_config_header`: use
  `qstar.configure_file`
- `.qs` fragments: use `.qst`
- `qstar.workspace`: use nearest ancestor `qstar.lua`
- top-level `include_dirs`, `public_include_dirs`, `private_include_dirs`,
  `system_include_dirs`
- top-level `public_headers`, `private_headers`
- top-level `modules`
- `hcl_include_dirs`: use `lang.cale.public_include_dirs` or
  `lang.cale.private_include_dirs`
- top-level `cflags`, `cxxflags`, `cxx_standard`

## Extension Seal

The VSCode extension is a release artifact, not a committed source artifact.
`node_modules/`, `dist/`, and `.vsix` files must stay untracked.

```txt
cd qstar/editors/vscode/qstar
npm run check
npm run package:vsix
code --install-extension dist/qstar-vscode-0.2.0.vsix --force
```

The extension owns `.qst` and the exact filename `qstar.lua`; it must not claim
`.qs` or `qstar.workspace`.

## Pilot Readiness

QStar v0.3 is suitable for a standalone local systems-project pilot when:

- the project can express boot/package/smoke steps through generic
  `qstar.custom_target`, `qstar.run_target`, and `qstar.stage`;
- external tool paths are declared through profile/tool capability policy;
- failure replay and action logs are sufficient to identify compile, link,
  objcopy, package, or smoke failures;
- the project accepts that QStar is still standalone and not yet the public
  `cale build` implementation.

This is not a full package manager seal and not a Cale language/frontend seal.
