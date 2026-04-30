#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>

#include "embedded_irx.h"

/* These headers are generated at build time by bin2c from the
   corresponding files in irx/. Each one defines:
       unsigned char  <name>_irx[]            __attribute__((aligned(16)));
       unsigned int   size_<name>_irx;
   They are included exactly once in this translation unit so the arrays
   end up as ordinary globals in the ELF. */
#include "netplay_irx.h"
#include "sjpcm2_irx.h"
#include "mcsave_irx.h"

struct EmbeddedEntry
{
    const char          *name;
    const unsigned char *data;
    unsigned int         size;
};

/* CDVD.IRX is intentionally not embedded here. The iaddis CDVD.IRX
   registers RPC id CDVD_IRX (0x0B001337) and CDVD_Init() in cdvd_rpc.c
   spins forever in SifBindRpc waiting for that server. On NetherSX2
   (and any setup where the rom-resident CDVD service is the only one
   available) loading the custom IRX after main.cpp's cdvdInit() bound
   to the rom-resident RPC tends to deadlock the IOP. Skipping the
   embed lets IOPLoadModule("CDVD.IRX") fail through, so CDVD_Init()
   never gets called and boot continues to LIBSD/SJPCM2/MCSAVE. */
static const EmbeddedEntry s_embedded[] =
{
    { "NETPLAY.IRX", netplay_irx, sizeof(netplay_irx) },
    { "SJPCM2.IRX",  sjpcm2_irx,  sizeof(sjpcm2_irx)  },
    { "MCSAVE.IRX",  mcsave_irx,  sizeof(mcsave_irx)  },
};

static const char *path_basename(const char *path)
{
    const char *p = path;
    const char *base = path;

    while (*p)
    {
        if (*p == '/' || *p == '\\' || *p == ':')
        {
            base = p + 1;
        }
        p++;
    }

    return base;
}

static int eq_ci(const char *a, const char *b)
{
    while (*a && *b)
    {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

extern "C" int EmbeddedIrxFind(const char *path,
                               const unsigned char **out_data,
                               unsigned int         *out_size)
{
    if (!path) return -1;

    const char *base = path_basename(path);
    if (!*base) return -1;

    for (size_t i = 0; i < sizeof(s_embedded) / sizeof(s_embedded[0]); ++i)
    {
        if (eq_ci(base, s_embedded[i].name))
        {
            if (out_data) *out_data = s_embedded[i].data;
            if (out_size) *out_size = s_embedded[i].size;
            return 0;
        }
    }

    return -1;
}

extern "C" int EmbeddedIrxLoad(const unsigned char *data,
                               unsigned int         size,
                               int                  arg_len,
                               const char          *args)
{
    int result = 0;
    int ret;

    /* SifExecModuleBuffer transfers the IRX from EE RAM to IOP RAM and
       starts it. Returns the module ID on success. */
    ret = SifExecModuleBuffer((void *)data, size, arg_len, args, &result);
    if (ret < 0) return ret;
    return ret;
}
