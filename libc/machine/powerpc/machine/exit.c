#ifdef _XBOX
#include <unistd.h>
#include <stdio.h>

#include <xam.h>
#include <xboxkrnl.h>

typedef void (*fini_fn)(void);
extern fini_fn __xt_a[];
extern fini_fn __xt_z[];

__attribute__((section(".CRT$XTA"), used))
fini_fn __pico_xt_a[1] = { NULL };

__attribute__((section(".CRT$XTZ"), used))
fini_fn __pico_xt_z[1] = { NULL };

#pragma comment(linker, "/alternatename:__xt_a=__pico_xt_a")
#pragma comment(linker, "/alternatename:__xt_z=__pico_xt_z")

void
_exit(int code)
{
    for (fini_fn *fn = __xt_z; fn != __xt_a + 1;) {
        --fn;
        if (*fn)
            (*fn)();
    }
    
    fflush(stdout);
    DbgPrint("_exit: Exiting with code %d\n", code);
    /* A backward branch gives Xenia's function scanner an explicit end. */
    for (;;)
        XamTerminateTitle();
}
#endif