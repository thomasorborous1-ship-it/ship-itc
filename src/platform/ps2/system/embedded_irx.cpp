#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>

#include "embedded_irx.h"

/* These headers are generated at build time by bin2c from the
   corresponding files in irx/ or directly from $(PS2SDK)/iop/irx/.
   Each one defines:
       unsigned char  <name>_irx[]            __attribute__((aligned(16)));
       unsigned int   size_<name>_irx;
   They are included exactly once in this translation unit so the arrays
   end up as ordinary globals in the ELF. */
#include "netplay_irx.h"
#include "audsrv_irx.h"
#include "freesd_irx.h"

struct EmbeddedEntry
{
    const char          *name;
    const unsigned char *data;
    unsigned int         size;
};

/* On NetherSX2 (and likely any setup whose IOP is not a fully faithful
   real PS2) the iaddis custom IRX modules do load via
   SifExecModuleBuffer, but their RPC entry points either never come up
   or come up incompatibly with the rom-resident services already bound
   by main.cpp / the BIOS. The result is the EE-side init function
   (CDVD_Init, SjPCM_Init, MCSave_Init) spins forever in SifBindRpc and
   the boot deadlocks. We've already observed this with CDVD.IRX and
   the legacy SJPCM2.IRX.

   The audio IRX has been migrated to PS2DEV's audsrv (see
   src/modules/sjpcm/sjpcm_rpc.c). audsrv.irx is the standard modern
   audio service from $(PS2SDK)/iop/irx/audsrv.irx, embedded here via
   bin2c so it is available without needing the user to ship it next
   to the ELF.

   CDVD/MCSAVE remain not-embedded for the same compatibility reason
   as before: their custom RPC servers hang on emulators. NETPLAY.IRX
   stays embedded; it's gated on a bLoadedNetwork flag and won't try
   to load when the IP stack didn't come up. */
static const EmbeddedEntry s_embedded[] =
{
    { "NETPLAY.IRX", netplay_irx, sizeof(netplay_irx) },
    { "AUDSRV.IRX",  audsrv_irx,  sizeof(audsrv_irx)  },
    /* freesd is the PS2SDK-supplied SPU2 driver IRX, used as a
       universal fallback when rom0:LIBSD is absent (early Japanese
       models, some emulator setups). audsrv binds to its sceSd*
       exports the same way it would to LIBSD's. */
    { "FREESD.IRX",  freesd_irx,  sizeof(freesd_irx)  },
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
