/* Game data files are read from the EBOOT directory (user-supplied, extracted from their own disc). */
#include <malloc.h>
#include <stdio.h>
#include "rt.h"

uint8_t *load_file(const char *name, uint32_t *size)
{
    char path[320];
    snprintf(path, sizeof path, "%s%s", g_basedir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = memalign(64, n ? n : 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = 0; }
    fclose(f);
    if (size) *size = (uint32_t)n;
    return buf;
}
