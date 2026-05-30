#include "corpus.h"

/** C-only sample executable의 진입점이다. */
int main(void)
{
	return corpus_magic() == 42 ? 0 : 1;
}
