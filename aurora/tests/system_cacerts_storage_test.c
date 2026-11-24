#include "../../core/src/mg_core.h"
#include "../../core/tests/mg_storage_tests.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    int checks = 0, failed;
    if (argc != 2 || !mg_core_init()) return 2;
    failed = mg_storage_tests_run(argv[1], NULL, NULL, &checks);
    printf("%d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
