#!/bin/sh
set -eu

out=
dep=
src=
mode=compile

while [ "$#" -gt 0 ]; do
	case "$1" in
		-o)
			shift
			out=$1
			;;
		/out:*)
			out=${1#/out:}
			;;
		-MF)
			shift
			dep=$1
			;;
		-c)
			shift
			src=$1
			;;
		-E)
			mode=preprocess
			;;
	esac
	shift || break
done

test -n "$out"
mkdir -p "$(dirname "$out")"
{
	printf "fake-clang-object\n"
	printf "mode=%s\n" "$mode"
	printf "src=%s\n" "${src:-<none>}"
} > "$out"
if [ -n "$dep" ]; then
	mkdir -p "$(dirname "$dep")"
	printf "%s: %s\n" "$out" "${src:-qstar-generated-input}" > "$dep"
fi
