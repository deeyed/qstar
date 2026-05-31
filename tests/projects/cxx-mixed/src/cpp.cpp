#include "cpp.hpp"

/** C source에서 호출할 수 있는 C ABI wrapper smoke 함수다. */
extern "C" int
cpp_value(void)
{
	return qstar_project_cpp_value();
}
