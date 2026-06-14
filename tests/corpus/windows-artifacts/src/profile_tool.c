extern int profile_core_value(void);

int main(void)
{
	return profile_core_value() == 42 ? 0 : 1;
}
