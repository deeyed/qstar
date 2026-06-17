#include <stdio.h>

extern int rust_value(void);

int main(void)
{
    int value = rust_value();
    printf("rust-value=%d\n", value);
    return value == 77 ? 0 : 1;
}

