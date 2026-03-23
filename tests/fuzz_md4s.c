#include <stdint.h>
#include <stdlib.h>
#include "../md4s.h"

static void fuzz_callback(enum md4s_event event,
                          const struct md4s_detail *detail,
                          void *user_data)
{
    (void)event;
    (void)detail;
    (void)user_data;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct md4s_parser *p = md4s_create(fuzz_callback, NULL);
    if (!p) return 0;
    md4s_feed(p, (const char *)data, size);
    char *raw = md4s_finalize(p);
    free(raw);
    md4s_destroy(p);
    return 0;
}
