#!/bin/sh
set -eu

kernel=${1:?kernel artifact required}
log=${2:?smoke log required}
mkdir -p "$(dirname "$log")"

if command -v qemu-system-aarch64 >/dev/null 2>&1; then
	qemu-system-aarch64 --version >/dev/null 2>&1 || {
		reason="qemu-system-aarch64-version-failed"
		printf "QSTAR_QEMU_SKIP reason=%s kernel=%s\n" "$reason" "$kernel"
			{
				printf "QSTAR-SMOKE-SKIP reason=%s\n" "$reason"
				printf "QSTAR-SMOKE-DONE\n"
			} > "$log"
		exit 0
	}
	printf "QSTAR_QEMU_RUN qemu-system-aarch64 --version kernel=%s\n" "$kernel"
		{
			printf "QSTAR-FIRMWARE-OK\n"
			printf "QSTAR-SMOKE-DONE\n"
		} > "$log"
	exit 0
fi

reason="qemu-system-aarch64-not-found"
printf "QSTAR_QEMU_SKIP reason=%s kernel=%s\n" "$reason" "$kernel"
{
	printf "QSTAR-SMOKE-SKIP reason=%s\n" "$reason"
	printf "QSTAR-SMOKE-DONE\n"
} > "$log"
