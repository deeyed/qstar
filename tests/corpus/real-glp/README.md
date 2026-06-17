# Real GLP Compiler Corpus

This optional corpus validates standard Generic Language Provider fixtures with
real host compilers instead of fake compiler shims.

Run it with:

```sh
make qstar-real-glp-compiler-corpus-tests
```

Hosted maintainers can also run the optional Linux/macOS GitHub Actions lane:

```sh
gh workflow run "Real GLP Compiler Validation"
```

That lane installs/caches Rust and Zig, uploads environment and corpus logs as
artifacts, and is documented in `docs/real-glp-compiler-ci.md`.

The script skips a language when its compiler is not available on `PATH`.
Currently it checks:

- Rust static consumer: `rustc --crate-type staticlib` runs as a provider-owned
  final artifact action, and a C consumer executable links and runs it.
- Zig static artifact: `zig build-lib -static` runs as a provider-owned final
  artifact action with project-local Zig cache directories. On macOS the fixture
  uses `lang.zig.macos_min_version` with an explicit `*-macos` target base to
  avoid linker minimum-version warnings. C consumer linking is intentionally not
  part of this fixture because Zig 0.16.0 static archives may need `ranlib`
  post-processing before Apple ld consumes them.
- Zig executable: `zig build-exe` runs as a provider-owned final artifact action
  and the resulting executable is run by the fixture.

Both Stella and Ninja are exercised when `ninja` is available. Ninja-only
absence is reported as a skipped backend, not as a failure.
