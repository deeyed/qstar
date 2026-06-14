#!/bin/sh
set -eu

out=
case "${1:-}" in
rcs)
	shift
	out=${1:-}
	shift || true
	;;
*)
	printf 'fake-lib: expected rcs action\n' >&2
	exit 1
	;;
esac

test -n "$out" || {
	printf 'fake-lib: output path not found\n' >&2
	exit 1
}

mkdir -p "$(dirname "$out")"
{
	printf 'fake static library\n'
	for input do
		printf 'input=%s\n' "$input"
	done
} > "$out"
