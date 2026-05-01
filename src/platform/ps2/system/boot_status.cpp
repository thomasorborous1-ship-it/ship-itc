#include <stdarg.h>
#include "boot_status.h"

extern "C" void BootStatusLog(const char *fmt, ...)
{
    (void)fmt;
}

extern "C" void BootProbeReclaim(const char *label)
{
    (void)label;
}
