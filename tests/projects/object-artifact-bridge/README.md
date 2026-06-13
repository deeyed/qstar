# Object Artifact Bridge Corpus

This corpus seals backend parity for generated object artifacts.

- `src/AppDelegate.m` is not a QStar language-provider source.
- `qstar.custom_target "objc_object"` invokes a fake Objective-C compiler.
- The generated `.o` is declared with `qstar.output(..., {format = "object"})`.
- `qstar.executable`, `qstar.staticlib`, and `qstar.sharedlib` consume that object
  through `sources`.

The fake compiler intentionally emits an object through a package-local wrapper.
It proves the bridge contract without adding Objective-C as a QStar compile
provider.
