#include "config.h"

int generated_value(void);

/** generated config/source corpus executable의 smoke entrypoint다. */
int
main(void)
{
	return generated_value() - QSTAR_PROJECT_VALUE;
}
