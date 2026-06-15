# QStar Model

QStar models one package-root build graph.

Core entities:

- project metadata from `qstar.project`
- tool role bundles from `qstar.toolset`
- reusable option bundles from `qstar.config`
- artifact targets such as executables, libraries, and tests
- generated actions and configured files
- run targets, groups, stages, and install actions
- package-relative source, header, generated, stage, and install paths

Language semantics are intentionally narrow. QStar directly classifies C, C++,
and ASM sources. Other languages enter through generated object artifacts:
`qstar.custom_target` creates `qstar.output(path, {format = "object"})`, and a
consumer target lists that object in `sources`.

Toolsets do not infer execution environment policy. If a compiler needs special
argv items, the project writes them in `qstar.config` or target-local fields.
