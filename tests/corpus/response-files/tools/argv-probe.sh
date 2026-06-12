#!/bin/sh
set -eu

out=$1
arg1=$2
arg2=$3
arg3=$4
arg4=$5

mkdir -p "$(dirname "$out")"
{
	printf 'arg1=%s\n' "$arg1"
	printf 'arg2=%s\n' "$arg2"
	printf 'arg3=%s\n' "$arg3"
	printf 'arg4=%s\n' "$arg4"
} > "$out"
