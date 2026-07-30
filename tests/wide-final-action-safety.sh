#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
tmp=${TMPDIR:-/tmp}/qstar-wide-final-safety-$$
build=$tmp/build-sanitize
bin=$build/bin/qstar

cleanup() {
  rc=$?
  if test "$rc" -ne 0; then
    printf 'qstar-wide-final-safety: preserved=%s\n' "$tmp" >&2
  else
    rm -rf "$tmp"
  fi
}
trap cleanup EXIT HUP INT TERM

rm -rf "$tmp"
mkdir -p "$tmp"

case "$("$cc" --version 2>/dev/null | head -n 1)" in
  *clang*|*Clang*|*GCC*|*gcc*)
    ;;
  *)
    printf 'qstar-wide-final-safety: status=skipped reason=unsupported-compiler cc=%s\n' \
      "$cc"
    exit 0
    ;;
esac

make -C "$root" -j2 BUILD_DIR="$build" CC="$cc" \
  CFLAGS="-g -O1 -pipe -fsanitize=address,undefined -fno-omit-frame-pointer" \
  LDLIBS="-lm -fsanitize=address,undefined" > "$tmp/build.out" 2> "$tmp/build.err"

ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
QSTAR_TEST_QSTAR="$bin" \
QSTAR_WIDE_FINAL_OBJECTS=1000 \
QSTAR_WIDE_FINAL_DIRECT_SOURCES=1000 \
CC="$cc" \
sh "$root/tests/wide-final-action.sh" > "$tmp/corpus.out" 2> "$tmp/corpus.err"

grep -F -q "status=ok final_objects=1000 direct_sources=1000" "$tmp/corpus.out"
printf 'qstar-wide-final-safety: status=ok sanitizers=address,undefined direct_sources=1000 mixed_objectlib=1000\n'
