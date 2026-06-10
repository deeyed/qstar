volatile unsigned long qstar_firmware_counter;

void qstar_firmware_main(void)
{
	qstar_firmware_counter = 0x58UL;
	for (;;) {
		break;
	}
}
