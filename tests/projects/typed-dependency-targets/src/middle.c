#if QSTAR_BASE_USAGE != 1 || QSTAR_API_USAGE != 1 || QSTAR_VENDOR_USAGE != 1
#error typed compile usage did not propagate through the public dependency closure
#endif

#ifdef QSTAR_PRIVATE_USAGE
#error private usage requirement leaked through a public dependency
#endif

int vendor_value(void);

int
middle_value(void)
{
	return vendor_value() + 1;
}
