#include "../system_cacerts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    char error[512];
    char *result;
    if (argc != 6) return 2;
    result = acl_system_cacerts_prepare(argv[1], argv[2], argv[3], strcmp(argv[4], "-") ? argv[4] : NULL,
                                        strcmp(argv[5], "-") ? argv[5] : NULL,
                                        NULL, NULL, NULL, error, sizeof(error));
    if (error[0]) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    puts(result ? result : "builtin");
    free(result);
    return 0;
}
