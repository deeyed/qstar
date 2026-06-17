# Hosted Real GLP Compiler CI

QStar의 GLP real compiler corpus는 기본 push/PR gate가 아니라 선택 실행하는
`workflow_dispatch` lane이다. 목적은 로컬 Mac에서 통과한 Rust/Zig provider 검증을
GitHub hosted Linux/macOS 환경에서도 반복 가능하게 만드는 것이다.

## Workflow

파일:

```text
.github/workflows/real-glp-compiler-validation.yml
```

이 workflow는 수동 실행 전용이다.

```sh
gh workflow run "Real GLP Compiler Validation"
```

또는 특정 Zig 버전과 compiler strictness를 지정할 수 있다.

```sh
gh workflow run "Real GLP Compiler Validation" \
  -f zig_version=0.16.0 \
  -f require_compilers=true
```

## Matrix

현재 hosted matrix는 두 축이다.

| Host | Runner | CC/CXX | 목적 |
| --- | --- | --- | --- |
| `linux-x86_64` | `ubuntu-latest` | `gcc`/`g++` | Linux hosted real compiler 검증 |
| `macos` | `macos-latest` | `cc`/`c++` | macOS hosted real compiler 검증 |

각 job은 다음을 수행한다.

1. submodule 포함 checkout
2. Ninja와 C toolchain 확인
3. Rust toolchain cache 복원 및 `rustc` 준비
4. Zig toolchain cache 복원 및 pinned Zig 설치
5. `make all`
6. `make qstar-real-glp-compiler-corpus-tests`
7. environment, install status, stdout/stderr, 최종 status artifact upload

## Rust

Rust는 hosted runner의 `rustup`을 우선 사용한다.

Workflow는 `rustup`으로 stable toolchain을 minimal 구성으로 설치한 뒤 stable을
default로 지정한다. 이 명령은 workflow 파일 안에만 두고, public QStar DSL 문서에서는
QStar CLI surface와 혼동되지 않도록 rustup flag를 직접 예시하지 않는다.

cache 대상은 다음과 같다.

```text
~/.cargo/bin
~/.cargo/registry
~/.rustup/settings.toml
~/.rustup/toolchains
~/.rustup/update-hashes
```

`rustup`이 없지만 `rustc`가 이미 있으면 그 compiler를 그대로 쓴다. 둘 다 없으면
`rust-install.status`에 skip 사유를 기록한다.

## Zig

Zig는 workflow input `zig_version`으로 pin한다. 기본값은 로컬 real corpus 기준과 같은
`0.16.0`이다.

다운로드 대상은 runner OS/arch에서 결정한다.

| Runner | Zig target |
| --- | --- |
| `Linux-X64` | `x86_64-linux` |
| `Linux-ARM64` | `aarch64-linux` |
| `macOS-X64` | `x86_64-macos` |
| `macOS-ARM64` | `aarch64-macos` |

cache path:

```text
.cache/zig/${runner.os}-${runner.arch}-${zig_version}
```

Zig 설치 실패, 다운로드 실패, unsupported runner는 `zig-install.status`에 기록된다.

## Skip Policy

`require_compilers=true`가 기본값이다. 이 값에서는 `rustc` 또는 `zig`가 설치되지 않거나
corpus가 compiler missing skip을 기록하면 job이 실패한다. Hosted lane은 “실제로 compiler가
있는 환경에서 검증한다”는 판단 자료이기 때문이다.

`require_compilers=false`로 실행하면 compiler 설치 실패/부재를 artifact에 남기고, corpus의
기존 skip 정책을 따른다. 이 모드는 runner image 변화나 Zig archive availability를 조사할 때
사용한다.

## Artifacts

각 matrix job은 다음 artifact를 업로드한다.

```text
qstar-real-glp-linux-x86_64
qstar-real-glp-macos
```

artifact에는 다음 파일이 들어간다.

```text
environment.txt
rust-install.status
rustc-version.txt
zig-install.status
zig-version.txt
make-all.log
real-glp-compiler-corpus.out
real-glp-compiler-corpus.err
real-glp-compiler-corpus.status
```

실패했을 때는 `real-glp-compiler-corpus.err`와 각 install status를 먼저 본다. Corpus 자체가
성공했는지는 `real-glp-compiler-corpus.status`의 `status=ok`로 확인한다.

## Corpus Contract

Workflow가 실행하는 대상은 다음 Make target이다.

```sh
make qstar-real-glp-compiler-corpus-tests
```

이 target은 실제 `rustc`와 `zig`가 PATH에 있을 때 다음을 검증한다.

- Rust staticlib provider final action: `rustc --crate-type staticlib`
- Rust staticlib를 C consumer가 link/run하는 경로
- Zig static artifact provider final action: `zig build-lib -static`
- Zig executable provider final action: `zig build-exe`
- Zig action-local cache env와 action-log/replay redaction
- Stella/Ninja backend parity

`qstar init --use-language=rust|zig` real scaffold 검증은 별도 target
`make qstar-real-language-init-scaffold-tests`가 담당한다. Q232 hosted lane은 우선 provider
compiler corpus를 고정하고, init scaffold hosted lane은 이후 독립적으로 붙일 수 있다.
