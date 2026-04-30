#pragma once

/* Boot-time debug helper. When DEBUG_BOOT_SCREEN is on, BootStatusLog()
   writes a numbered, padded status line to a fixed position at the
   bottom of the BIOS debug screen so we always know which step the
   boot is currently on - even if the rest of scr_printf output got
   wrapped, scrolled or partially clobbered by GS framebuffer activity.
   It also prints the same message inline at the current cursor so we
   keep a scrollable history above the status line.

   When DEBUG_BOOT_SCREEN is off it falls back to printf, which the
   user can still see on real PS2 / ps2link / a kit. */

#ifdef __cplusplus
extern "C" {
#endif

void BootStatusLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
