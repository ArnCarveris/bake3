#include "examples_c_pkg_amalgamate_shared.h"

int examples_shared_value(void) {
    return 42;
}

#ifdef EXAMPLES_SHARED_EXTRA
int examples_shared_extra(void) {
    return 7;
}
#endif

