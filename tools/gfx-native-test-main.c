#include "../core/src/mg_core.h"
#include "../core/src/dvm/mg_dvm.h"
#include "../core/tests/mg_gfx_tests.h"

#include <stdio.h>

int main(void)
{
    int checks = 0;
    int failed;
    if (!mg_core_init() || !mg_dvm_start(NULL, 0)) {
        fputs("Could not initialize the core or Dalvik runtime.\n", stderr);
        return 1;
    }
    failed = mg_gfx_tests_run(NULL, NULL, &checks);
    failed += mg_gfx_natives_tests_run(NULL, NULL, &checks);
    fprintf(stderr, "%d graphics checks, %d failures.\n", checks, failed);
    return failed != 0 || checks == 0;
}
