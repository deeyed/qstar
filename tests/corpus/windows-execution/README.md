# Windows Execution Corpus

This corpus is the Windows native execution baseline for the MSYS2 UCRT64 GCC
lane. It intentionally tests real build and run behavior instead of only
Windows-like dry-run contracts.

Covered flows:

- C executable build and run
- static library plus executable build and run
- response-file driven compile and link
- generated object artifact bridge
- install prefix smoke for executable and static library artifacts
- stage layout smoke for executable, static library, and generated object bridge
  outputs
- slash-normalized install and stage manifest records

MSVC, clang-cl, `.lib`, `.dll`, import library, and PDB policy are covered by
separate Windows artifact contract tests until those paths become native release
gates.
