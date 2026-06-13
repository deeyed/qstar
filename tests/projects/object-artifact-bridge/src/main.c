int objc_bridge_value(void);

int
main(void)
{
	return objc_bridge_value() == 41 ? 0 : 1;
}
