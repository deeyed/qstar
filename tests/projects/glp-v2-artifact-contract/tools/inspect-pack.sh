#!/bin/sh
set -eu

runtime=$1
metadata=$2
resources=$3
link=$4
output=$5

test -f "$runtime"
test -f "$metadata"
test -f "$resources/index.txt"
test -f "$link"
mkdir -p "$(dirname "$output")"
printf 'ok\n' > "$output"
