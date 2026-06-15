#if !defined(__QSTAR_PACKAGE_FLOW__)
#error "__QSTAR_PACKAGE_FLOW__ missing"
#endif

.section .text.start
.global _start
_start:
	b qstar_module_main
