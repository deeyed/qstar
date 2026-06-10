#!/bin/sh
set -eu

format=
input=
output=

while [ "$#" -gt 0 ]; do
	case "$1" in
		-O)
			shift
			format=$1
			;;
		*)
			if [ -z "$input" ]; then
				input=$1
			else
				output=$1
			fi
			;;
	esac
	shift || break
done

test "$format" = binary
test -n "$input"
test -n "$output"
mkdir -p "$(dirname "$output")"
{
	printf "RAW-BINARY\n"
	cat "$input"
} > "$output"
