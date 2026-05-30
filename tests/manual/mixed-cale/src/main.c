int cale_plugin_value(void);

/** C/Cale mixed sample executable의 C 진입점이다. */
int main(void)
{
	return cale_plugin_value() == 9 ? 0 : 1;
}
