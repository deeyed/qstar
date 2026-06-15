#include <stdio.h>

#include "winexec.h"

int
main(void)
{
	printf("windows-execution app core=%d\n", winexec_core_value());
	return winexec_core_value() == 42 ? 0 : 1;
}
