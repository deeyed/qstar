#!/bin/sh
set -eu

artifact=
layout=
manifest=
mode=fast
result=generated/check/result.txt
verify=0

while [ "$#" -gt 0 ]; do
	case "$1" in
		--artifact)
			artifact=$2
			shift 2
			;;
		--layout)
			layout=$2
			shift 2
			;;
		--manifest)
			manifest=$2
			shift 2
			;;
		--mode)
			mode=$2
			shift 2
			;;
		--result)
			result=$2
			shift 2
			;;
		--verify)
			verify=1
			shift
			;;
		*)
			printf 'unknown argument: %s\n' "$1" >&2
			exit 2
			;;
	esac
done

if [ -z "$artifact" ] || [ -z "$layout" ]; then
	printf 'artifact and layout are required\n' >&2
	exit 2
fi

if [ ! -f "$artifact" ]; then
	printf 'missing artifact: %s\n' "$artifact" >&2
	exit 3
fi

if [ ! -f "$layout/artifacts/payload.artifact" ]; then
	printf 'missing staged artifact: %s\n' "$layout/artifacts/payload.artifact" >&2
	exit 4
fi

if [ -n "$manifest" ] && [ ! -f "$manifest" ]; then
	printf 'missing manifest input: %s\n' "$manifest" >&2
	exit 5
fi

if ! cmp "$artifact" "$layout/artifacts/payload.artifact" >/dev/null 2>&1; then
	printf 'artifact content mismatch\n' >&2
	exit 6
fi

case "$result" in
	*/*) mkdir -p "${result%/*}" ;;
esac

{
	printf 'WORKFLOW_OK\n'
	printf 'mode=%s\n' "$mode"
	printf 'verify=%s\n' "$verify"
	printf 'artifact=%s\n' "$artifact"
	printf 'layout=%s\n' "$layout"
} > "$result"

cat "$result"
