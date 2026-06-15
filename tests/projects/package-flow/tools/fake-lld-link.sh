#!/bin/sh
set -eu

out=
subsystem=0
entry=0
nodefault=0

for arg in "$@"; do
	case "$arg" in
		/out:*)
			out=${arg#/out:}
			;;
		/subsystem:console)
			subsystem=1
			;;
		/entry:payload_main)
			entry=1
			;;
		/nodefaultlib)
			nodefault=1
			;;
	esac
done

test -n "$out"
test "$subsystem" = 1
test "$entry" = 1
test "$nodefault" = 1
mkdir -p "$(dirname "$out")"
{
	printf "MZ\n"
	printf "QSTAR-PE\n"
	printf "out=%s\n" "$out"
} > "$out"
