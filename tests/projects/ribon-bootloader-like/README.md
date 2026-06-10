# Ribon-like Bootloader Corpus

This self-contained project is QStar's bootloader-grade release corpus. It models
two practical flows without adding UEFI, Raspberry Pi, or QEMU as dedicated
QStar keywords:

- AArch64 freestanding C + preprocessed assembly + linker script.
- ELF to raw image conversion through `qstar.custom_target` + `qstar.cli`.
- RPi-style copy-only staging with `qstar.stage`.
- QEMU/serial smoke through `qstar.run_target` and `marker_log`.
- UEFI PE/COFF output naming and ESP staging through profile/link policy.

The tools in `tools/` are deterministic fake tools. A real Ribon package can
replace them through `Cale.toml` profile fields without changing the target
model.

Useful commands:

```sh
qstar --file qstar.lua dry-run //:kernel
qstar --file qstar.lua stage //:rpi
qstar --file qstar.lua build //:qemu_smoke
qstar --file qstar.lua dry-run //:uefi_boot --profile uefi-x64
qstar --file qstar.lua stage //:esp --profile uefi-x64
```

The QEMU wrapper invokes `qemu-system-aarch64 --version` when the tool exists and
writes a deterministic serial marker. If QEMU is absent it writes a skip reason
instead. Both paths include `RIBON-SMOKE-DONE` so QStar can validate
`run_target` marker plumbing without requiring QEMU in every developer
environment.
