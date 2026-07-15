#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

int
main(void)
{
#if defined(_WIN32)
	Sleep(3000);
#else
	sleep(3);
#endif
	return 0;
}
