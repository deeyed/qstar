# Windows Artifacts Corpus

This corpus records the Q173 Windows artifact implementation contract and the
Q174 executable/static-library regression gate before Windows shared library
support is implemented.

It is intentionally separate from `tests/corpus/response-files`:

- `response-files` verifies MSVC response escaping and fake Windows command
  execution on non-Windows hosts.
- `windows-artifacts` documents target shapes and artifact roles for `.exe`,
  static `.lib`, runtime `.dll`, import `.lib`, install, stage, and backend
  parity.

Useful current commands:

```sh
qstar --file tests/corpus/windows-artifacts/qstar.lua check
make qstar-windows-prep-tests
```

Current expected behavior:

- `//:tool` plans an explicit `.exe` primary artifact.
- `//:core` plans an explicit static `.lib` primary artifact.
- `//:named_tool` and `//:named_core` prove target-local
  `artifact_name` for `.exe` and static `.lib`.
- The `windows_fake` toolset builds those artifacts on non-Windows hosts with
  package-local fake tools; this validates QStar naming, Stella/Ninja lowering,
  stage, and install layout without claiming native Windows tool support.
- `//:plugin` is a Windows `sharedlib` contract target. Current dry-run output
  may show a provisional primary `.dll` path, but that is not the complete
  Windows shared library implementation. Backend execution/lowering is not
  considered sealed until the multi-output runtime `.dll` plus import `.lib`
  artifact map lands.
- `//:layout` stages the currently supported executable and static library
  primary artifacts under `bin/` and `lib/`.

Future expected behavior after Windows sharedlib implementation:

- `qstar.target_file("//:plugin")` resolves to `plugin.dll`.
- `qstar.target_file("//:plugin", { artifact = "import_lib" })` resolves to
  `plugin.lib`.
- Link consumers use `plugin.lib` automatically.
- Stage/install can place `plugin.dll` under `bin/` and `plugin.lib` under
  `lib/`.
- PDB/debug artifacts stay opt-in and are not staged or installed implicitly.

This corpus does not claim official Windows support and must not be used as a
substitute for the hosted Windows native alpha workflow.
