#include "config.h"

int generated_value(void);

int main(void) { return generated_value() == QSTAR_GENERATED_VALUE ? 0 : 1; }
