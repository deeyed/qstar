#ifndef QSTAR_PROJECT_CPP_HPP
#define QSTAR_PROJECT_CPP_HPP

#ifndef QSTAR_PROJECT_CXX_FLAG
#error missing QSTAR_PROJECT_CXX_FLAG
#endif

/** QStar C++ corpus에서 C++ flag가 적용된 값을 반환한다. */
static inline int qstar_project_cpp_value(void)
{
	return 40 + QSTAR_PROJECT_CXX_FLAG;
}

#endif
