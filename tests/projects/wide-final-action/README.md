# Wide Final Action Fixture

This fixture is materialized into a temporary directory by
`tests/wide-final-action.sh`. The script creates the requested number of
direct sources and prebuilt object inputs, so the repository does not carry
thousands of generated fixture files.

The project deliberately uses only generic QStar primitives:

- artifact targets and `qstar.objectlib`;
- generated and imported object inputs;
- built-in C plus GLP v1/v2 providers;
- toolset response policies and POSIX/MSVC materialization styles.

The fake tools validate response-file atom order, missing or duplicate object
inputs, and output ownership without requiring a host language compiler.
