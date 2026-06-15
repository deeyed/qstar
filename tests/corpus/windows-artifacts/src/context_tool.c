extern int context_core_value(void);

int main(void)
{
	return context_core_value() == 42 ? 0 : 1;
}
