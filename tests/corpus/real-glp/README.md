# Real GLP Compiler Corpus

This optional corpus validates standard Generic Language Provider fixtures with
real host compilers instead of fake compiler shims.

Run it with:

```sh
make qstar-real-glp-compiler-corpus-tests
```

The script skips a language when its compiler is not available on `PATH`.
Currently it checks:

- Rust: `rustc --crate-type lib --emit=obj` builds a provider object through
  `lang.rust.crate_type`, QStar archives it, and a C consumer executable links
  and runs it.
- Zig: `zig build-obj` builds a provider object with project-local Zig cache
  directories, QStar archives it, and a C consumer executable links and runs it.

Both Stella and Ninja are exercised when `ninja` is available. Ninja-only
absence is reported as a skipped backend, not as a failure.
