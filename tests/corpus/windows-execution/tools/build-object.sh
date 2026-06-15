#!/bin/sh
set -eu

input=$1
output=$2

mkdir -p "$(dirname "$output")"
${QSTAR_WINDOWS_EXECUTION_CC:-${CC:-gcc}} -Iinclude -Wall -Wextra -c "$input" -o "$output"
