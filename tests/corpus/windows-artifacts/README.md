# Windows Artifacts Corpus

This corpus records the Q173 Windows artifact implementation contract, the Q174
executable/static-library regression gate, and the Q223 Windows shared-library
Graph IR gate before Windows shared-library backend lowering is implemented.
In short, it keeps the Q174 executable/static-library regression gate and adds
the Q223 sharedlib artifact-map gate.

It is intentionally separate from `tests/corpus/response-files`:

- `response-files` verifies MSVC response escaping and fake Windows command
  execution on non-Windows hosts.
- `windows-artifacts` documents target shapes and artifact roles for `.exe`,
  static `.lib`, runtime `.dll`, import `.lib`, install, stage, and backend
  parity.

Useful current commands:

```sh
qstar --file tests/corpus/windows-artifacts/qstar.lua --qstar-internal-platform windows check
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
- `//:plugin` is a Windows `sharedlib` contract target. `explain`, `dry-run`,
  `query`, and `list-targets --format json` expose its artifact map: primary
  runtime `plugin.dll` plus secondary import `plugin.lib`.
- `//:layout` stages the currently supported executable and static library
  primary artifacts under `bin/` and `lib/`.
- `//:plugin_layout` dry-runs stage layout for
  `qstar.target_file("//:plugin")` and
  `qstar.target_file("//:plugin", { artifact = "import_lib" })`.

Current Q223 selector behavior:

- `qstar.target_file("//:plugin")` resolves to `plugin.dll`.
- `qstar.target_file("//:plugin", { artifact = "import_lib" })` resolves to
  `plugin.lib`.
- Stage dry-run can place `plugin.dll` under `bin/` and `plugin.lib` under
  `lib/`.
- Install dry-run reports `role=sharedlib artifact=runtime` for the `.dll` and
  `role=import_lib artifact=import_lib` for the import library.
- Unknown selector names fail with a diagnostic that lists known artifacts.

Future expected behavior after Windows sharedlib lowering:

- Link consumers use `plugin.lib` automatically.
- Stella and Ninja produce both `plugin.dll` and `plugin.lib` from one
  `link-shared` action.
- Non-dry-run stage/install can copy the produced files.
- PDB/debug artifacts stay opt-in and are not staged or installed implicitly.

This corpus does not claim official Windows support and must not be used as a
substitute for the hosted Windows native alpha workflow.
