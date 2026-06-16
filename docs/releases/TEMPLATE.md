# QStar vX.Y.Z Beta N

QStar vX.Y.Z Beta N is a prerelease of QStar as a standalone build system.

This release is a beta. Publish only the platform artifacts that have passed
the release gate for this tag. Keep candidate and planned platforms clearly
marked as candidate, validation underway, or planned.

## Highlights

- Stella native executor:
- Ninja backend:
- Authoring DSL:
- Documentation and editor support:

## Install

```sh
tar -xzf qstar-vX.Y.Z-beta.N-macos-arm64.tar.gz -C "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
qstar --version
```

Expected version:

```txt
qstar X.Y.Z-beta.N
```

## Release Asset Checklist

- Runtime tarball:
- `SHA256SUMS`:
- Docs/wiki installed under `share/doc/qstar/wiki`:
- Manpages installed under `share/man/man1` and `share/man/man5`:
- macOS codesign verification:
- Linux x86_64 candidate dry-run: `QSTAR_RELEASE_PLATFORM=linux-x86_64`
  packaging, ELF `file(1)`, `ldd(1)`, installed docs/man smoke, extracted
  tarball `qstar --version`/docs/man/file/ldd smoke:
- Linux release-candidate artifact upload:
  `qstar-linux-x86_64-release-candidate-dry-run`:
- Linux medium performance artifacts: Ubuntu gcc/clang
  `medium_project_gate ...`, `runner=posix_spawn event_wait=poll`,
  Stella/Ninja summary:
- Linux daemon socket smoke: optional `workflow_dispatch`
  `daemon_socket_smoke=true`, required only if this release claims
  Linux daemon behavior is release-backed:
- VSCode extension included: no, unless this section explicitly says otherwise.

## Validate

```sh
qstar init app /tmp/qstar-hello
qstar --file /tmp/qstar-hello/qstar.lua build //:app --progress plain
/tmp/qstar-hello/build/qstar/out/___app/app
```

## Platform Status

| Host platform | Status |
| --- | --- |
| macOS arm64 | Beta release artifact |
| Linux x86_64 | Beta release artifact if built by Ubuntu gcc/clang CI release workflow or clean Linux host |
| Windows | Planned or candidate validation; publish only after native release gate |

## Notes

- The Makefile remains QStar's canonical bootstrap and release build path.
- QStar self-host remains a release gate candidate and backend parity check.
- Linux artifacts require `.github/workflows/linux-validation.yml` to be green for
  gcc and clang, plus `QSTAR_RELEASE_PLATFORM=linux-x86_64` package dry-run,
  ELF/ldd sanity, install docs/man smoke, extracted tarball smoke, explicit
  Ninja backend parity, and medium perf artifact collection before publication.
- Windows artifacts require native platform validation before publication.
- QStar is licensed under Apache-2.0. Lua vendor license text is preserved in
  `LICENSE/lua.txt`.
