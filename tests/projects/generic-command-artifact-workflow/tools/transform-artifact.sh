#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
	printf 'usage: transform-artifact.sh input output\n' >&2
	exit 2
fi

input=$1
output=$2

case "$output" in
	*/*) mkdir -p "${output%/*}" ;;
esac

cat "$input" > "$output"
