/* Standalone: gcc -std=gnu11 -O2 mg_numeric_locale_test.c -o numeric-test && ./numeric-test */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include "../../src/bionic/mg_libc_str.c"

static int guest_errno;
void mg_set_errno(int e) { guest_errno = e; }
void *mg_g_malloc(size_t n) { (void)n; abort(); }
char *mg_g_strdup(const char *s) { (void)s; abort(); }
size_t mg_strlcpy(char *d, const char *s, size_t n) { (void)d; (void)s; (void)n; abort(); }

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static void check_numbers(void)
{
    const char *s = ".01x";
    char *end;
    guest_errno = 0;
    CHECK(fabs(mg_gd(g_strtod_l(s, &end, NULL)) - 0.01) < 1e-12);
    CHECK(end == s + 3 && guest_errno == 0);
    s = "1.5x";
    CHECK(fabs(mg_gf(g_strtof(s, &end)) - 1.5f) < 1e-6);
    CHECK(end == s + 3 && guest_errno == 0);
    CHECK(fabs(mg_gf(g_strtof_l(s, &end, NULL)) - 1.5f) < 1e-6 && end == s + 3);
    CHECK(fabs(mg_gld(g_strtold_l(s, &end, NULL)) - 1.5) < 1e-12 && end == s + 3);
    CHECK(fabs(mg_gd(g_atof("12.5")) - 12.5) < 1e-12);
    s = "1,5";
    CHECK(mg_gd(g_strtod(s, &end)) == 1.0 && end == s + 1);
    s = "x";
    CHECK(mg_gd(g_strtod(s, &end)) == 0.0 && end == s);
    guest_errno = GE_INVAL;
    CHECK(mg_gd(g_strtod("1.", &end)) == 1.0 && *end == '\0' && guest_errno == GE_INVAL);
    guest_errno = 0;
    s = "1e9999";
    CHECK(isinf(mg_gd(g_strtod_l(s, &end, NULL))) && *end == '\0' && guest_errno == GE_RANGE);
    guest_errno = 0;
    s = "1e-9999";
    CHECK(mg_gd(g_strtod(s, &end)) == 0.0 && *end == '\0' && guest_errno == GE_RANGE);
}

int main(void)
{
#ifdef _WIN32
    int old_mode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
    char saved[128];
    const char *old = setlocale(LC_NUMERIC, NULL);
    if (!old || strlen(old) >= sizeof(saved)) return 2;
    strcpy(saved, old);
    if (!setlocale(LC_NUMERIC, "Russian_Russia.1251")) {
        fprintf(stderr, "Russian numeric locale unavailable\n");
        return 2;
    }
#else
    locale_t comma = newlocale(LC_NUMERIC_MASK, "ru_RU.UTF-8", (locale_t)0);
    locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t old;
    if (!comma || !c_locale) {
        fprintf(stderr, "ru_RU.UTF-8 numeric locale unavailable\n");
        return 2;
    }
    old = uselocale(comma);
    if (!old) return 2;
#endif
    CHECK(strcmp(localeconv()->decimal_point, ",") == 0);
    check_numbers();
    CHECK(strcmp(localeconv()->decimal_point, ",") == 0);
#ifdef _WIN32
    CHECK(setlocale(LC_NUMERIC, "C") != NULL);
    check_numbers();
    setlocale(LC_NUMERIC, saved);
    _configthreadlocale(old_mode);
#else
    CHECK(uselocale((locale_t)0) == comma);
    CHECK(uselocale(c_locale) == comma);
    check_numbers();
    uselocale(old);
    freelocale(comma);
    freelocale(c_locale);
#endif
    if (!failures) puts("numeric locale regression passed");
    return failures ? 1 : 0;
}
