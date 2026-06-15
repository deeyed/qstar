# QStar v0.2 Release Candidate Seal

status: v0.2 release candidate

This historical gate is kept as a release-process marker. The old authoring
surface from that era is no longer documented here; current public docs must
teach the generic toolset/config DSL only.

Current equivalent surfaces:

- `qstar.toolset` for compiler/archive/link/response-file/external tool policy
- `qstar.config` for reusable compile and link options
- `qstar.custom_target` for generated package artifacts
- `qstar.run_target` for generic smoke wrappers
- `qstar.stage` for copy-only package trees
- object artifact bridge for unsupported source languages

Historical gate names retained for test continuity:

- `qstar-v0.2-rc-tests`
- `qstar-systems-corpus-tests`
