# Response File Corpus

This corpus keeps response-file, argv-vector process, and Windows-prep policy
coverage in one small project.

Useful commands:

```sh
qstar --file tests/corpus/response-files/qstar.lua build //:all
qstar --file tests/corpus/response-files/qstar.lua --profile windows-msvc dry-run //:windows_app
```
