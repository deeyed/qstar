#!/bin/sh
set -eu

out=
script=
map_seen=0
defs=

while [ "$#" -gt 0 ]; do
	case "$1" in
		-o)
			shift
			out=$1
			;;
		-T)
			shift
			script=$1
			;;
		-Wl,-Map=*)
			map_seen=1
			;;
		--defsym=*)
			defs="$defs $1"
			;;
	esac
	shift || break
done

test -n "$out"
test -n "$script"
test "$map_seen" = 1
mkdir -p "$(dirname "$out")"
{
	printf "ELF\n"
	printf "RIBON-KERNEL\n"
	printf "script=%s\n" "$script"
	printf "defs=%s\n" "$defs"
} > "$out"
