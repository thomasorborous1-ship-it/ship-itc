#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <debug.h>

#ifndef DEBUG_BOOT_SCREEN
#define DEBUG_BOOT_SCREEN 0
#endif

#include "boot_status.h"

#if DEBUG_BOOT_SCREEN

/* The BIOS debug font is 8 px tall and the framebuffer init_scr() sets
   up gives ~28 lines on the standard 640x480 GS mode. We pin the
   status line to the last printable row. */
#define BOOT_STATUS_ROW 27

static int g_BootStatusStep = 0;

extern "C" void BootStatusLog(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Drop a trailing newline if present so the status line doesn't
       get pushed off-screen by the cursor advance. */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    {
        buf[--len] = 0;
    }

    /* Print to the regular flow first so we keep history above. */
    int sx = scr_getX();
    int sy = scr_getY();
    scr_printf("%s\n", buf);

    /* Update the pinned status line at the bottom of the screen.
       Save & restore cursor so the regular flow keeps moving. */
    int rx = scr_getX();
    int ry = scr_getY();
    g_BootStatusStep++;
    scr_setXY(0, BOOT_STATUS_ROW);
    /* Pad to a fixed width so any previous longer status is overwritten. */
    scr_printf("STEP %3d: %-60.60s", g_BootStatusStep, buf);
    scr_setXY(rx, ry);

    (void)sx;
    (void)sy;
}

#else /* !DEBUG_BOOT_SCREEN */

extern "C" void BootStatusLog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

#endif
