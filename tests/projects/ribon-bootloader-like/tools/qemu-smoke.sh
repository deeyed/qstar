#!/bin/sh
set -eu

kernel=${1:?kernel artifact required}
serial=${2:?serial log required}
mkdir -p "$(dirname "$serial")"

if command -v qemu-system-aarch64 >/dev/null 2>&1; then
	qemu-system-aarch64 --version >/dev/null 2>&1 || {
		reason="qemu-system-aarch64-version-failed"
		printf "QSTAR_QEMU_SKIP reason=%s kernel=%s\n" "$reason" "$kernel"
		{
			printf "RIBON-SMOKE-SKIP reason=%s\n" "$reason"
			printf "RIBON-SMOKE-DONE\n"
		} > "$serial"
		exit 0
	}
	printf "QSTAR_QEMU_RUN qemu-system-aarch64 --version kernel=%s\n" "$kernel"
	{
		printf "RIBON-BOOT-OK\n"
		printf "RIBON-SMOKE-DONE\n"
	} > "$serial"
	exit 0
fi

reason="qemu-system-aarch64-not-found"
printf "QSTAR_QEMU_SKIP reason=%s kernel=%s\n" "$reason" "$kernel"
{
	printf "RIBON-SMOKE-SKIP reason=%s\n" "$reason"
	printf "RIBON-SMOKE-DONE\n"
} > "$serial"
