#include "../core/src/mg_core.h"
#include "../core/src/dvm/mg_dvm.h"
#include "../core/tests/mg_gfx_tests.h"

int main(void)
{
    int checks = 0;
    if (!mg_core_init() || !mg_dvm_start(NULL, 0)) return 1;
    return mg_gl_natives_tests_run(NULL, NULL, &checks) || !checks;
}
