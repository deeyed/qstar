#include <stdio.h>

int
main(void)
{
	FILE *f;

	f = fopen("retry.flag", "r");
	if (f) {
		fclose(f);
		return 0;
	}
	f = fopen("retry.flag", "w");
	if (f)
		fclose(f);
	return 1;
}
