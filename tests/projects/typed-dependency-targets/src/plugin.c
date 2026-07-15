#if QSTAR_BASE_USAGE != 1 || QSTAR_API_USAGE != 1 || QSTAR_VENDOR_USAGE != 1
#error typed compile usage did not reach the shared library
#endif

int vendor_value(void);

int
plugin_value(void)
{
	return vendor_value() + 2;
}
