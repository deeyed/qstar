#include <stdio.h>

__declspec(dllimport) int winexec_plugin_value(void);

int main(void)
{
	int value = winexec_plugin_value();
	printf("windows-execution plugin=%d\n", value);
	return value == 64 ? 0 : 1;
}
