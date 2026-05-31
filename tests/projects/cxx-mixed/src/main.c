#ifndef QSTAR_PROJECT_C_FLAG
#error missing QSTAR_PROJECT_C_FLAG
#endif

int cpp_value(void);

/** C/C++ mixed target의 link smoke entrypoint다. */
int
main(void)
{
	return cpp_value() - 42;
}
