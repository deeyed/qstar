#!/bin/sh
set -eu

config=$1
out=$2
test -f "$config"
mkdir -p "$(dirname "$out")"
cat > "$out" <<'SRC'
#include "config.h"

int generated_value(void) { return QSTAR_GENERATED_VALUE; }
SRC
