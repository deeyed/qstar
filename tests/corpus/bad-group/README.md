# Bad Group Corpus Fixture

This fixture intentionally fails. It verifies that `qstar.target_file` cannot
reference a deps-only `qstar.group` target because groups have no artifact.

Manual diagnostic check:

```sh
./build/bin/qstar --file tests/corpus/bad-group/qstar.lua check
```
