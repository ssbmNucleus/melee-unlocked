/* The owner's 11-18 UI audit picks are the defaults (the Squad Strike label's width, place and lift;
 * the arrows; the page and row text). The other previews stay off: mu_ui_fix() is always 0 and
 * mu_ui_num() always its default, so nothing here reads the environment in a release build.
 * Building with -DMU_UI_PREVIEWS brings back MELEE_TEST_UI=<ids> and MELEE_TEST_UI_NUM=<numbers>
 * for measuring another preview on captures. */
#ifndef MU_TEST_UI_H
#define MU_TEST_UI_H
#ifndef MU_UI_PREVIEWS
static inline int mu_ui_fix(const char* id) { (void) id; return 0; }
static inline float mu_ui_num(int index, float fallback) { (void) index; return fallback; }
#else
#include <string.h>
const char* mu_test_getenv(const char* name);

static inline int mu_ui_fix(const char* id)
{
    static const char* list;
    static int read;
    const char* p;
    size_t n = strlen(id);
    if (!read) { list = mu_test_getenv("MELEE_TEST_UI"); read = 1; }
    for (p = list; p != NULL && *p;) {
        const char* end = strchr(p, ',');
        size_t len = end ? (size_t) (end - p) : strlen(p);
        if (len == n && memcmp(p, id, n) == 0) return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

static inline float mu_ui_num(int index, float fallback)
{
    static const char* list;
    static int read;
    const char* v;
    int i = 0;
    if (!read) { list = mu_test_getenv("MELEE_TEST_UI_NUM"); read = 1; }
    for (v = list; v != NULL && *v; ++i) {
        float sign = 1.0f, val = 0.0f, scale = 0.0f;
        int digits = 0;
        if (*v == '-') { sign = -1.0f; v++; }
        for (; (*v >= '0' && *v <= '9') || *v == '.'; v++) {
            if (*v == '.') { scale = 1.0f; continue; }
            val = val * 10.0f + (float) (*v - '0');
            if (scale > 0.0f) scale *= 10.0f;
            digits++;
        }
        if (i == index) return digits ? sign * (scale > 0.0f ? val / scale : val) : fallback;
        while (*v && *v != ',') v++;
        if (*v == ',') v++;
    }
    return fallback;
}
#endif
#endif
