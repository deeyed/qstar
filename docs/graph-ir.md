# QStar Graph IR

Graph IR is the canonical internal representation after Lua evaluation and
before Stella/Ninja lowering.

Current top-level record classes:

- project metadata
- toolsets
- configs
- targets
- generated actions
- stages
- target families

Target records store package-relative source/header paths, dependency labels,
merged config options, selected toolset label, link options, link inputs, and
artifact naming. Toolset records store role argv vectors for `c`, `cxx`, `asm`,
`archive`, and `link`, plus response-file and external path tool policy.

Graph dumps are diagnostic artifacts. They must describe the current generic
DSL and must not reintroduce removed authoring surfaces.
