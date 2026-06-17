#include <stdio.h>

extern int zig_value(void);

int main(void)
{
    int value = zig_value();
    printf("zig-value=%d\n", value);
    return value == 88 ? 0 : 1;
}

