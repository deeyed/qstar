# Response File Corpus

This corpus keeps response-file, argv-vector process, and Windows-prep policy
coverage in one small project.

Useful commands:

```sh
qstar --file tests/corpus/response-files/qstar.lua build //:all
qstar --file tests/corpus/response-files/qstar.lua --profile windows-msvc dry-run //:windows_app
qstar --file tests/corpus/response-files/qstar.lua --profile windows-msvc-fake build //:windows_rsp
qstar --file tests/corpus/response-files/qstar.lua --profile windows-msvc-static-fake build //:windows_static
```

`windows-msvc-fake` uses a package-local fake `clang-cl` script so non-Windows
hosts can still verify MSVC response-file escaping for spaces, quotes,
backslashes, trailing spaces, and Windows-like argv option paths. The corpus also
checks `.exe` artifact naming through target-local `artifact_name` and
profile-level `artifact_names`, `/LIBPATH:...`, and `.lib` system library
spelling without claiming native Windows support. It also checks explicit
static `.lib` planning through profile `artifact_names`.

`windows-msvc-static-fake` adds a package-local fake archiver so Stella and Ninja
can build a real `windows_static.lib` fixture on non-Windows hosts. This proves
QStar artifact naming, archive action construction, action logs, and Ninja
lowering without claiming native `lib.exe` or `llvm-lib` compatibility.

`windows_plugin` is intentionally unsupported for Windows-like profiles. Stella
and Ninja must reject it with the same diagnostic that names the deferred
runtime `.dll`, import `.lib`, and PDB/debug artifact policy.
