#!/bin/sh
set -eu

artifact=${1:?module artifact required}
log=${2:?smoke log required}
mkdir -p "$(dirname "$log")"

test -f "$artifact"
printf "QSTAR_PACKAGE_SMOKE artifact=%s\n" "$artifact"
{
	printf "QSTAR-PACKAGE-OK\n"
	printf "QSTAR-SMOKE-DONE\n"
} > "$log"
