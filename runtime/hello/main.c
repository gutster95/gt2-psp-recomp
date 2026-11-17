/* Toolchain smoke test: prints a line on the PSP debug screen. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>

PSP_MODULE_INFO("GT2HELLO", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

int main(void)
{
    pspDebugScreenInit();
    pspDebugScreenPrintf("GT2 PSP recomp: toolchain OK\n");
    for (int i = 0; i < 300; i++)
        sceDisplayWaitVblankStart();
    sceKernelExitGame();
    return 0;
}
