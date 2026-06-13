# Response File Corpus

This corpus keeps response-file, argv-vector process, and Windows-prep policy
coverage in one small project.

Useful commands:

```sh
qstar --file tests/corpus/response-files/qstar.lua build //:all
qstar --file tests/corpus/response-files/qstar.lua --profile windows-msvc dry-run //:windows_app
qstar --file tests/corpus/response-files/qstar.lua --profile windows-msvc-fake build //:windows_rsp
```

`windows-msvc-fake` uses a package-local fake `clang-cl` script so non-Windows
hosts can still verify MSVC response-file escaping, `.exe` artifact naming,
`/LIBPATH:...`, and `.lib` system library spelling without claiming native
Windows support.
