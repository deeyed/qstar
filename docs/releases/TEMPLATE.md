# QStar vX.Y.Z Beta N

QStar vX.Y.Z Beta N is a prerelease of QStar as a standalone build system.

This release is a beta. Publish only the platform artifacts that have passed
the release gate for this tag. Keep planned platforms clearly marked as planned
or validation underway.

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
- VSCode extension included: no, unless this section explicitly says otherwise.

## Validate

```sh
qstar init c-app /tmp/qstar-hello
qstar --file /tmp/qstar-hello/qstar.lua build //:app --progress plain
/tmp/qstar-hello/build/qstar/out/___app/app
```

## Platform Status

| Host platform | Status |
| --- | --- |
| macOS arm64 | Beta release artifact |
| Linux | Validation underway through Ubuntu gcc/clang CI |
| Windows | Planned validation |

## Notes

- The Makefile remains QStar's canonical bootstrap and release build path.
- QStar self-host remains a release gate candidate and backend parity check.
- Linux artifacts require `.github/workflows/linux-validation.yml` to be green for
  gcc and clang, plus an install docs/man smoke, before publication.
- Windows artifacts require native platform validation before publication.
- QStar is licensed under Apache-2.0. Lua vendor license text is preserved in
  `LICENSE/lua.txt`.
