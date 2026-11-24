#include "../../src/apk/mg_apk.h"
#include "../../src/drivers/mg_driver.h"
#include "../../src/mg_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Inspection uses the real driver registry, without starting an Android runtime. */
const mg_driver mg_driver_java = {.name = "java"};
const mg_driver mg_driver_unity = {.name = "unity"};
const mg_driver mg_driver_native_activity = {.name = "native-activity"};
int mg_core_arm_state(void) { return 0; }
bool mg_flag(const char *name) { (void)name; return false; }
void mg_locale_parts(char *language, size_t language_size, char *region, size_t region_size)
{
    mg_strlcpy(language, "en", language_size);
    mg_strlcpy(region, "", region_size);
}

int main(int argc, char **argv)
{
    mg_apk_info *info = calloc(1, sizeof(*info));
    if (!info || argc < 2) return 2;
    if (!mg_apk_read_app(argv[1], (const char *const *)(argv + 2), argc - 2, NULL, info)) {
        fprintf(stderr, "%s\n", info->error);
        free(info);
        return 1;
    }
    printf("%s\t%d\t%s\t%d\n", info->engine, info->runnable, info->abi, info->nsplits);
    free(info);
    return 0;
}
