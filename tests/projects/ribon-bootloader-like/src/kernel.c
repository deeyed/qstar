volatile unsigned long ribon_boot_counter;

void ribon_kernel_main(void)
{
	ribon_boot_counter = 0x58UL;
	for (;;) {
		break;
	}
}
