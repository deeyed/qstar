# Systems Firmware Corpus

This self-contained project is QStar's systems/firmware release corpus. It models
practical low-level artifact flows without adding UEFI, Raspberry Pi, or QEMU as
dedicated QStar keywords:

- AArch64 freestanding C + preprocessed assembly + linker script.
- ELF to raw image conversion through `qstar.custom_target` + `qstar.cli`.
- RPi-style copy-only staging with `qstar.stage`.
- External smoke validation through `qstar.run_target` and `expect`.
- UEFI PE/COFF output naming and ESP staging through target-local artifact and link policy.

The tools in `tools/` are deterministic fake tools. A real package can replace
them through `qstar.toolset` and `qstar.config` declarations in `qstar.lua`
without changing the target model.

Useful commands:

```sh
qstar --file qstar.lua dry-run //:kernel
qstar --file qstar.lua stage //:rpi
qstar --file qstar.lua build //:qemu_smoke
qstar --file qstar.lua --target x86_64-pc-windows-msvc --toolchain clang dry-run //:uefi_boot
qstar --file qstar.lua --target x86_64-pc-windows-msvc --toolchain clang stage //:esp
```

The QEMU wrapper invokes `qemu-system-aarch64 --version` when the tool exists and
writes a deterministic smoke log. If QEMU is absent it writes a skip reason
instead. Both paths include `QSTAR-SMOKE-DONE` so QStar can validate
`run_target` expectation plumbing without requiring QEMU in every developer
environment.
