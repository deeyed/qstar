#!/bin/sh
set -eu

input=$1
output=$2
tmp="${output}.c"

test -f "$input"
mkdir -p "$(dirname "$output")"
cat > "$tmp" <<'EOF_C'
int
objc_bridge_value(void)
{
	return 41;
}
EOF_C

${CC:-cc} -fPIC -c "$tmp" -o "$output"
