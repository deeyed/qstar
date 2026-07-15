# QStar Model

QStar models one package-root build graph.

Core entities:

- project metadata from `qstar.project`
- tool role bundles from `qstar.toolset`
- reusable option bundles from `qstar.config`
- artifact targets such as executables, libraries, and tests
- typed dependency targets for artifact-free interfaces, package-local prebuilt
  artifacts, and executable tool paths
- generated actions and configured files
- run targets, groups, stages, and project-defined commands
- package-relative source, header, generated, stage, and explicit export paths

Language semantics are intentionally narrow. QStar directly classifies C, C++,
and ASM sources through preloaded provider namespaces. Other languages enter
through activated GLP source units or generated object artifacts. A provider
source unit lowers to an object-producing action template owned by the consuming
target. A v1 provider can also declare final artifact hooks so pure provider-owned
`executable`, `staticlib`, or `sharedlib` targets are lowered through the
provider compiler instead of QStar's native C-style linker/archive action. A v2
provider declares final input ownership and named file/tree artifacts so native
and foreign-provider objects can share one provider-owned final action. The
object artifact bridge still uses `qstar.custom_target` plus
`qstar.output(path, {format = "object"})`.

Toolsets do not infer execution environment policy. If a compiler needs special
argv items, the project writes them in `qstar.config` or target-local fields.
Imported targets follow the same boundary: `artifact_kind`, artifact ids, and
filename suffixes never infer flags. Consumer compile/link requirements are
explicit `compile_usage`/`link_usage` options and rebuild inputs.
