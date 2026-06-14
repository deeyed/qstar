# Windows Artifacts Corpus

This corpus records the Q173 Windows artifact implementation contract before
Windows shared library support is implemented.

It is intentionally separate from `tests/corpus/response-files`:

- `response-files` verifies MSVC response escaping and fake Windows command
  execution on non-Windows hosts.
- `windows-artifacts` documents target shapes and artifact roles for `.exe`,
  static `.lib`, runtime `.dll`, import `.lib`, install, stage, and backend
  parity.

Useful current commands:

```sh
qstar --file tests/corpus/windows-artifacts/qstar.lua check
qstar --file tests/corpus/windows-artifacts/qstar.lua --profile windows-msvc dry-run //:tool
qstar --file tests/corpus/windows-artifacts/qstar.lua --profile windows-msvc dry-run //:core
qstar --file tests/corpus/windows-artifacts/qstar.lua --profile windows-msvc dry-run //:layout
```

Current expected behavior:

- `//:tool` plans an explicit `.exe` primary artifact.
- `//:core` plans an explicit static `.lib` primary artifact.
- `//:plugin` is a Windows `sharedlib` contract target. Current dry-run output
  may show a provisional primary `.dll` path, but that is not the complete
  Windows shared library implementation. Backend execution/lowering is not
  considered sealed until the multi-output runtime `.dll` plus import `.lib`
  artifact map lands.
- `//:layout` stages the currently supported executable and static library
  primary artifacts only.

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
