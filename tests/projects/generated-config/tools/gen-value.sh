#!/bin/sh
set -eu

out=$1
mkdir -p "$(dirname "$out")"
cat > "$out" <<'SRC'
/** generated source corpus에서 생성된 값을 반환한다. */
int generated_value(void) { return 17; }
SRC
