#if !defined(__RIBON_BOOT__)
#error "__RIBON_BOOT__ missing"
#endif

.section .text.boot
.global _start
_start:
	b ribon_kernel_main
