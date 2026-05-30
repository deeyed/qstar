#include "corpus.h"

/** 두 정수의 합을 계산하는 QStar sample 함수다. */
int corpus_add(int lhs, int rhs)
{
	return lhs + rhs;
}

/** sample executable과 test가 공유하는 고정 값을 반환한다. */
int corpus_magic(void)
{
	return corpus_add(17, 25);
}
