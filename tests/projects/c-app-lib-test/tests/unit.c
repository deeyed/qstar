#include "corpus.h"

/** C app corpus static library 값을 검증하는 unit test entrypoint다. */
int
main(void)
{
	return corpus_value() == 31 ? 0 : 1;
}
