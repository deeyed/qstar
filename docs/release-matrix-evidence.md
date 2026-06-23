# Three-OS Release Matrix Evidence Ledger

Status: Q260 three-OS release evidence matrix.

이 문서는 QStar의 macOS, Linux, Windows release evidence를 한곳에 모으는 ledger다.
목적은 "어떤 OS를 지원한다고 말할 수 있는가"가 아니라, 각 OS가 현재 어느 수준의
release evidence를 갖고 있는지를 명확히 구분하는 것이다.

QStar 1.0 판단은 기능 목록만으로 결정하지 않는다. 각 host의 release artifact가 실제로
만들어지고, 업로드되고, 다시 내려받아 smoke된 근거가 있어야 한다. 특히 Windows는
`v0.7.19-beta`에서 처음 published GitHub Release asset evidence를 얻었지만, 같은 gate를
다음 beta/RC tag와 v1 candidate tag에서 반복하기 전까지 official support로 승격하지 않는다.

## Evidence Levels

| Level | Meaning | v1 판단에서의 의미 |
| --- | --- | --- |
| Local smoke | 현재 checkout 또는 local install tree에서 build/test/package smoke가 통과했다. | 개발 freshness evidence다. Release artifact 존재를 증명하지 않는다. |
| Candidate artifact | CI 또는 local release script가 archive/zip을 만들고 추출 smoke까지 수행했다. | 사용자가 받을 수 있는 layout에 가까운 evidence지만, GitHub Release에 게시된 asset 검증은 아니다. |
| Published GitHub Release asset | GitHub Release에 올라간 asset을 다시 내려받아 checksum, layout, docs/man, provider, init/build smoke를 통과했다. | v1 host support의 최소 release-backed evidence다. 단일 성공만으로 반복성까지 증명하지는 않는다. |

## Current Matrix

| Host | Current status | Highest evidence level | Current evidence | Remaining v1 condition |
| --- | --- | --- | --- | --- |
| macOS arm64 | Public beta release host | Published GitHub Release asset expected for current beta line; local package/download smoke and codesign gates are active | `qstar-v0.7.19-beta-macos-arm64.tar.gz` is listed in the current release assets. Local release gate checks `qstar --version`, installed docs/wiki/manpages, prefix layout, and Darwin ad-hoc codesign. | Fresh clean-tag asset upload/download smoke must be green on the v1 candidate tag. Evidence must be recorded alongside the three-OS release matrix. |
| Linux x86_64 | Public beta release host | Published GitHub Release asset with hosted validation history | Hosted Ubuntu lane produces the Linux tarball from the release workflow or clean Linux host. Current docs require gcc/clang validation, Ninja parity, install smoke, package dry-run, uploaded asset download smoke, `file(1)`, `ldd(1)`, docs/wiki/man checks, and medium performance artifact collection. The current beta line lists `qstar-v0.7.19-beta-linux-x86_64.tar.gz`; prior hosted evidence records `linux_release_asset status=published` and `download_smoke=ok`. | Fresh Linux published-asset download smoke must be green on the v1 candidate tag, with the decision artifact preserved. |
| Windows x86_64 | Validation-backed beta candidate, not official support | Published GitHub Release asset seed evidence | Q254 recorded the first release-backed Windows asset evidence for `v0.7.19-beta`: workflow `https://github.com/deeyed/qstar/actions/runs/27935992747`, release `https://github.com/deeyed/qstar/releases/tag/v0.7.19-beta`, asset `qstar-v0.7.19-beta-windows-x86_64.zip`, `windows_release_asset status=published`, and `download_smoke=ok`. The downloaded smoke verified `qstar --version`, docs/man lookup, bundled providers, Zig provider vendoring, `qstar init app`, Stella build, and Ninja build from the extracted zip. | Repeat the same release-mutating `publish_windows_asset=true` gate on the next beta/RC tag and again on the v1 candidate tag. A normal `--ref main` freshness run is useful but does not replace release-backed repetition. |

## Windows Repetition Rule

Windows is the highest-risk host for v1 because its release evidence only became
release-backed in the `v0.7.19-beta` line. The current state is "evidence held",
not "official support".

The next beta/RC tag must run:

```sh
gh workflow run windows-validation.yml \
  --ref <tag> \
  -f release_tag=<tag> \
  -f publish_windows_asset=true
```

The run counts only if the uploaded artifact
`qstar-windows-x86_64-published-release-asset` contains
`windows-hosted-release-decision.txt` with:

```txt
windows_release_asset status=published
download_smoke=ok
```

The downloaded smoke must continue to verify:

- uploaded zip checksum through merged `SHA256SUMS`
- extracted `bin/qstar.exe`
- `qstar --version`
- docs/man lookup
- bundled language providers
- `qstar init app`
- Zig provider vendoring
- Stella build
- Ninja build

The same gate must run again for the final v1 candidate tag.

## Release Gate Mapping

| Gate | Evidence level | Notes |
| --- | --- | --- |
| `make check` | Local smoke | Broad local regression. It is necessary but not release-backed. |
| `make qstar-public-beta-release-tests` | Candidate artifact | Builds current-host runtime package and validates install/docs/man layout. On macOS it also checks codesign. |
| `make qstar-public-beta-download-smoke` | Published GitHub Release asset | Re-downloads uploaded assets and validates checksum/layout/docs/man/runtime smoke for the selected platform. |
| Linux `publish_linux_asset=true` workflow | Published GitHub Release asset | Ubuntu hosted release mutation plus downloaded Linux tarball smoke. |
| Windows `publish_windows_asset=true` workflow | Published GitHub Release asset | Windows/MSYS2 hosted release mutation plus downloaded Windows zip smoke. |
| `make qstar-windows-release-asset-smoke-tests` | Candidate artifact or contract-only local smoke | On Windows it creates/extracts the zip; on non-Windows hosts it remains a contract smoke. |

## Canonical References

- `docs/qstar-v1-readiness.md`: v1 gap report and host support conditions.
- `docs/qstar-v0.8-readiness.md`: 0.8 beta readiness and Windows beta candidate path.
- `docs/public-beta-release.md`: release package and uploaded-asset smoke contract.
- `docs/linux-validation.md`: Linux hosted validation and publish/download evidence.
- `docs/windows-native-alpha.md`: Windows beta candidate and release publication evidence.
- `docs/releases/v0.7.19-beta.md`: current beta patch release note and asset list.

## Q260 Decision

Q260 does not promote any OS support level by itself. It only centralizes the
evidence ledger and adds drift guards so future release notes cannot imply that
candidate artifacts, local smoke, and published GitHub Release assets are the
same thing.

Current v1 release-matrix verdict:

- macOS arm64: public beta release host; v1 requires fresh published-asset smoke on the v1 candidate.
- Linux x86_64: public beta release host with hosted validation history; v1 requires fresh published-asset smoke on the v1 candidate.
- Windows x86_64: release-backed beta evidence exists for `v0.7.19-beta`, but official support requires repeated `publish_windows_asset=true` evidence on the next beta/RC tag and the v1 candidate tag.
