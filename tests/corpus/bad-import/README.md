# Bad Import Corpus Fixture

This fixture intentionally fails. It verifies that `qstar.import_module` rejects
file paths and explains the folder-based module convention.

Manual diagnostic check:

```sh
./build/bin/qstar --file tests/corpus/bad-import/qstar.lua check
```
