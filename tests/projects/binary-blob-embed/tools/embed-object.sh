#!/bin/sh
set -eu

input=$1
output=$2
bytes=$(wc -c < "$input" | tr -d ' ')
tmp="${output}.c"

mkdir -p "$(dirname "$output")"
cat > "$tmp" <<EOF_C
int qstar_embedded_payload_size = $bytes;
EOF_C
${CC:-cc} -c "$tmp" -o "$output"
