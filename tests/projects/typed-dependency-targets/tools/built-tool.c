#include <stdio.h>

int
main(int argc, char **argv)
{
	FILE *in, *out;
	char buffer[4096];
	size_t n;

	if (argc != 3)
		return 2;
	in = fopen(argv[1], "rb");
	if (!in)
		return 3;
	out = fopen(argv[2], "wb");
	if (!out) {
		fclose(in);
		return 4;
	}
	while ((n = fread(buffer, 1, sizeof(buffer), in)) != 0) {
		if (fwrite(buffer, 1, n, out) != n) {
			fclose(in);
			fclose(out);
			return 5;
		}
	}
	fclose(in);
	return fclose(out) == 0 ? 0 : 6;
}
