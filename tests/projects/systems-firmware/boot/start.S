#if !defined(__QSTAR_FIRMWARE__)
#error "__QSTAR_FIRMWARE__ missing"
#endif

.section .text.boot
.global _start
_start:
	b qstar_firmware_main
