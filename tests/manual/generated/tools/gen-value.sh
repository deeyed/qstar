#!/bin/sh
set -eu

out=$1
mkdir -p "$(dirname "$out")"
cat > "$out" <<'SRC'
int generated_value(void)
{
	return 42;
}
SRC
