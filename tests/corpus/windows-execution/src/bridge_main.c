#include <stdio.h>

#include "winexec.h"

int
main(void)
{
	printf("windows-execution bridge=%d\n", winexec_bridge_value());
	return winexec_bridge_value() == 77 ? 0 : 1;
}
