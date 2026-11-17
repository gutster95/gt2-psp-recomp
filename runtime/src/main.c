/* GT2 PSP runtime entry point. */
#include <malloc.h>
#include <string.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include "rt.h"
#include "gpu.h"
#include "cdrom.h"
#include "sio.h"

PSP_MODULE_INFO("GT2PSP", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(4096);
PSP_HEAP_SIZE_KB(-2048);

static PsxCpu g_cpu;

static int load_exe(void)
{
    uint32_t size;
    uint8_t *exe = load_file("SCUS_944.88", &size);
    if (!exe || size < 0x800 || memcmp(exe, "PS-X EXE", 8)) return 0;
    uint32_t pc0 = *(uint32_t *)(exe + 0x10), taddr = *(uint32_t *)(exe + 0x18), tsize = *(uint32_t *)(exe + 0x1C);
    memcpy(g_ram + (taddr & 0x1FFFFF), exe + 0x800, tsize);
    logf_("SCUS_944.88: pc0=%08x text=%08x size=%08x\n", (unsigned)pc0, (unsigned)taddr, (unsigned)tsize);
    free(exe);
    return 1;
}

int main(int argc, char **argv)
{
    char dir[256] = "";
    if (argc > 0 && argv[0]) {
        strncpy(dir, argv[0], sizeof dir - 1);
        char *s = strrchr(dir, '/');
        if (s) s[1] = 0; else dir[0] = 0;
    }
    log_init(dir);
    logf_("GT2 PSP runtime, base dir '%s'\n", dir);

    g_ram = memalign(64, PSX_RAM_SIZE);
    if (!g_ram) fatal(0, "out of memory (RAM)");
    memset(g_ram, 0, PSX_RAM_SIZE);
    gpu_init();
    io_init();
    cdrom_init();
    sio_init();
    if (!load_exe()) fatal(0, "cannot load SCUS_944.88 from the EBOOT directory");

    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.r[29] = 0x801FFF00u;            /* SYSTEM.CNF STACK */
    g_cpu.cop0[12] = 0x40000401u;          /* SR: COP2 enabled, IEc, IM2 */
    run_program(&g_cpu);
    fatal(&g_cpu, "program finished");
    return 0;
}
