# QStar License Notices

This directory is the standalone QStar license/notice bundle. It exists so the
`qstar/` tree can be moved into an independent repository without relying on the
Cale repository root `LICENSE/` directory.

## Vendor Licenses

- `lua.txt`: Lua license notice. QStar uses the official Lua repository as a git
  submodule at `vendor/lua` after extraction. In the current Cale monorepo
  layout, the path is `qstar/vendor/lua`.

License 전문은 요약문으로 대체하지 않는다. 새 vendor import가 생기면 upstream
license 전문과 provenance를 이 directory 안에 추가한다.
