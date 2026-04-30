#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <debug.h>
#include <sio.h>

#ifndef DEBUG_BOOT_SCREEN
#define DEBUG_BOOT_SCREEN 0
#endif

#include "boot_status.h"

/* Mirror every BootStatusLog into the EE SIO TX FIFO. PCSX2-derived
   emulators (incl. NetherSX2) capture SIO writes and surface them in
   their EE console log alongside the IOP "loadmodule:" lines, which is
   much easier to inspect than reading scr_printf output off a
   screenshot. On a real PS2 these bytes go out the (rarely connected)
   serial port, so this is harmless either way. */
static int g_BootSioInited = 0;

static void BootSioInitOnce(void)
{
    if (g_BootSioInited) return;
    /* 8N1 @ 38400 baud is the PS2-link / pcsx2 default. */
    sio_init(38400, 0, 0, 0, 0);
    g_BootSioInited = 1;
}

static void BootSioPuts(const char *s)
{
    BootSioInitOnce();
    while (*s)
    {
        sio_putc((unsigned char)*s);
        s++;
    }
}

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

    /* Mirror to SIO console (NetherSX2 / PCSX2 EE log). */
    BootSioPuts(buf);
    BootSioPuts("\n");

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

extern "C" void BootProbeReclaim(const char *label)
{
    /* Originally re-called init_scr() to take the GS back over with the
       BIOS debug font after each suspect call. That turned out to make
       the screen WORSE on NetherSX2 (re-init_scr after GS_InitGraph
       corrupted what was already there). Now this just logs the probe
       label - SIO output via BootStatusLog is the primary debug
       channel; the screen is best-effort. */
    BootStatusLog("[probe] %s", label);
}

#else /* !DEBUG_BOOT_SCREEN */

extern "C" void BootStatusLog(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* SIO is the primary debug channel - PCSX2/NetherSX2 capture this
       and surface it in the EE console log. */
    BootSioPuts(buf);
    /* Make sure we end with a newline so each log is on its own line. */
    size_t len = strlen(buf);
    if (len == 0 || (buf[len-1] != '\n' && buf[len-1] != '\r'))
    {
        BootSioPuts("\n");
    }
}

extern "C" void BootProbeReclaim(const char *label)
{
    BootStatusLog("[probe] %s", label);
}

#endif
