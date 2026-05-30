#include "corpus.h"

/** C-only sample library 동작을 검증하는 작은 test 진입점이다. */
int main(void)
{
	if (corpus_add(7, 5) != 12)
		return 1;
	if (corpus_magic() != 42)
		return 2;
	return 0;
}
