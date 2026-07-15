# Reusable Command Sets Fixture

This fixture keeps graph declaration authority in `qstar.lua` while a cached
QSM returns deeply immutable `qstar.command_spec` values. The root materializes
those values with `qstar.command_set`, then mixes them with one stable direct
`qstar.command` declaration.

The module generates 62 reusable specifications so command-name, alias, option,
environment, working-directory, step, default-command, and JSON listing paths
are exercised at a scale larger than hand-written smoke examples.
