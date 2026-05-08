#include "mainloop_net.h"
#include "mainloop_load.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boot_status.h"

#ifndef DEBUG_BOOT_SCREEN
#define DEBUG_BOOT_SCREEN 0
#endif

/* BOOTLOG: was a no-op. Re-enable as printf so the boot sequence
   shows up in the emulator's console log alongside the IOP
   loadmodule lines. Helps debug audio init and module loading. */
#define BOOTLOG(...) printf(__VA_ARGS__)
#define MENU_STARTDIR ""
#define NEWLIB_PORT_AWARE
#include <fileio.h>
#include <iopheap.h>
#include <libpad.h>
#include "libxpad.h"
#include "libxmtap.h"
#include <libmc.h>
#include <kernel.h>
#include "mainloop_debug.h"
#include "mainloop_shared.h"
#include "mainloop_state.h"
#include "types.h"
#include "mainloop.h"
#include "console.h"
#include "input.h"
#include "snes.h"
#include "rendersurface.h"
#include "file.h"
#include "dataio.h"
#include "prof.h"
#include "bmpfile.h"
#if 0
#include "font.h"
#else
#include "font.h"
#endif
#include "poly.h"
#include "texture.h"
#include "mixbuffer.h"
#include "wavfile.h"
#include "snstate.h"
#include "sjpcmbuffer.h"
#include "memcard.h"

#include "pathext.h"
#include "snppucolor.h"
#if 0
#include "version.h"
#endif
#include "emumovie.h"
extern "C" {
#include "cd.h"
#include "ps2dma.h"
#include "sncpu_c.h"
#include "snspc_c.h"
};

//#include "nespal.h"
#include "snes.h"
//#include "nesstate.h"

#include <sifrpc.h>
#include <loadfile.h>

extern "C" {
#include "ps2ip.h"
#include "netplay_ee.h"
#include "mcsave_ee.h"
};

extern "C" {
#include "hw.h"
#include "gs.h"
#include "gpfifo.h"
#include "gpprim.h"
};

extern "C" {
#include "titleman.h"
};

extern "C" {
#include "sjpcm.h"
#include "cdvd_rpc.h"
};

#include "embedded_irx.h"

extern "C" Int32 SNCPUExecute_ASM(SNCpuT *pCpu);


/* MAINLOOP_MEMCARD / NETPORT / STATEPATH / SNESSTATEDEBUG /
   NESSTATEDEBUG / HISTORY / MAXSRAMSIZE now live in
   mainloop_shared.h (already included above). */

#include "uiBrowser.h"
#include "uiNetwork.h"
#include "uiMenu.h"
#include "uiLog.h"
#include "emurom.h"

#include "mainloop_iop.h"
#include "mainloop_ui.h"

static int _LoadMcModule(const char *path, int argc, const char *argv)
{
    void *iop_mem;
    int ret;
	int fd;
	int size;

	fd= fioOpen(path, O_RDONLY);
	if (fd < 0)
	{
		return -1;
	}
	size = fioLseek(fd, 0, SEEK_END);
	fioClose(fd);

	printf("LoadMcModule %s (%d)\n", path, size);
    iop_mem = SifAllocIopHeap(size);
    if (iop_mem == NULL) {
		return -2;
    }
    ret = SifLoadIopHeap(path, iop_mem);
	ret=0;
    if (ret < 0) {
	    SifFreeIopHeap(iop_mem);
		return -3;
    }

	printf("SifLoadModuleBuffer %08X\n",(Uint32)iop_mem);
    ret = SifLoadModuleBuffer(iop_mem, argc, argv);
	printf("SifLoadModuleBuffer %d\n",ret);
    SifFreeIopHeap(iop_mem);
	return ret;
}

Int32 IOPLoadModule(const Char *pModuleName, Char **ppSearchPaths, int arglen, const char *pArgs)
{
    int ret = -1;
    char ModulePath[256];

    /* If we have a copy of this module embedded in the ELF, prefer it
       unconditionally. The host:/cdrom: search paths used by ppSearchPaths
       are not usable on emulators (NetherSX2 etc.) or on a stripped-down
       PS2, so falling through to SifLoadModule there is guaranteed to
       fail with -203 ("module not found"). */
    {
        const unsigned char *embed_data = NULL;
        unsigned int         embed_size = 0;

        if (EmbeddedIrxFind(pModuleName, &embed_data, &embed_size) == 0)
        {
            ret = EmbeddedIrxLoad(embed_data, embed_size, arglen, pArgs);
            if (ret >= 0)
            {
                ScrPrintf("IOP Load (embed): %s\n", pModuleName);
                return ret;
            }
            /* fall through to disk-based load if the embedded copy
               somehow refused to start. */
        }
    }

    if (ppSearchPaths)
    {
        // iterate through search paths
        while (*ppSearchPaths)
        {
			if (strlen(*ppSearchPaths) > 0)
			{
            	strcpy(ModulePath, *ppSearchPaths);
            	strcat(ModulePath, pModuleName);
				if (ModulePath[0] == 'm' && ModulePath[1]=='c')
				{
					ret = _LoadMcModule(ModulePath, arglen, pArgs);
				} else
				{
            		ret = SifLoadModule(ModulePath, arglen, pArgs);
				}

            	if (ret >= 0)
            	{
            	    // success!
					break;
            	}
			}

            ppSearchPaths++;
        }
    } else
    {
		strcpy(ModulePath, pModuleName);
        ret = SifLoadModule(ModulePath, arglen, pArgs);
    }


    if (ret >= 0)
    {
        // success!
		ScrPrintf("IOP Load: %s\n", ModulePath);
        return ret;
    } else
	{
		ScrPrintf("IOP Fail: %s %d\n", pModuleName, ret);
    	printf("IOP: Failed to load module '%s'\n", pModuleName);

    	// module not loaded
    	return -1;
	}
}

Char _MainLoop_BootDir[256];

Char *_MainLoop_IOPModulePaths[]=
{
	_MainLoop_BootDir,
    (char *)"host:",
    (char *)"cdrom:\\",
    (char *)"rom0:",
    NULL
};

Bool _MainLoop_bSjPCMReady = FALSE;
Bool _MainLoop_bMCSaveReady = FALSE;

void _MainLoopLoadModules(Char **ppSearchPaths)
{
	Bool bLoadedNetwork;

	#if 0
	if (!EEPuts_Init())
	{
		EEPuts_SetCallback(_MainLoop_Puts);
		IOPLoadModule("EEPUTS.IRX", ppSearchPaths, 0, NULL);
	}
	#endif

//    IOPLoadModule("rom0:SECRMAN", NULL, 0, NULL);

	BOOTLOG("[boot] rom0:XSIO2MAN: try\n");
	if (IOPLoadModule("rom0:XSIO2MAN", NULL, 0, NULL) >= 0)
	{
		BOOTLOG("[boot] rom0:XSIO2MAN OK\n");
		// use the X version of the iop libs
		BOOTLOG("[boot] rom0:XMTAPMAN: try\n");
	    if (IOPLoadModule("rom0:XMTAPMAN", NULL, 0, NULL) >= 0)
		{
			BOOTLOG("[boot] xmtapInit/xmtapPortOpen\n");
			xmtapInit(0);
			xmtapPortOpen(1,0);
			BOOTLOG("[boot] xmtapInit/xmtapPortOpen done\n");
		}
		BOOTLOG("[boot] rom0:XPADMAN: try\n");
	    if (IOPLoadModule("rom0:XPADMAN", NULL, 0, NULL) >= 0)
	    {
			BOOTLOG("[boot] xpadInit/InputInit(TRUE)\n");
	        xpadInit(0);
			InputInit(TRUE);
			BOOTLOG("[boot] xpadInit/InputInit done\n");
	    }

		BOOTLOG("[boot] rom0:XMCMAN: try\n");
	    IOPLoadModule("rom0:XMCMAN", NULL, 0, NULL);
		BOOTLOG("[boot] rom0:XMCSERV: try\n");
	    if (IOPLoadModule("rom0:XMCSERV", NULL, 0, NULL) >= 0)
		{
			BOOTLOG("[boot] MemCardInit (X)\n");
			MemCardInit();
			BOOTLOG("[boot] MemCardInit done (X)\n");
			#if MAINLOOP_MEMCARD
			MemCardCreateSave(_SramPath, _MainLoop_SaveTitle, TRUE);
			#endif
		}
	} else
	{
		BOOTLOG("[boot] rom0:XSIO2MAN failed - falling back\n");
		// use the regular versions
		BOOTLOG("[boot] rom0:SIO2MAN: try\n");
	    IOPLoadModule("rom0:SIO2MAN", NULL, 0, NULL);
		BOOTLOG("[boot] rom0:PADMAN: try\n");
	    if (IOPLoadModule("rom0:PADMAN", NULL, 0, NULL) >= 0)
	    {
			BOOTLOG("[boot] padInit/InputInit(FALSE)\n");
	        padInit(0);
			InputInit(FALSE);
			BOOTLOG("[boot] padInit/InputInit done\n");
	    }

		BOOTLOG("[boot] rom0:MCMAN: try\n");
	    IOPLoadModule("rom0:MCMAN", NULL, 0, NULL);
		BOOTLOG("[boot] rom0:MCSERV: try\n");
	    if (IOPLoadModule("rom0:MCSERV", NULL, 0, NULL) >= 0)
		{
			BOOTLOG("[boot] MemCardInit (regular)\n");
			MemCardInit();
			BOOTLOG("[boot] MemCardInit done (regular)\n");
			#if MAINLOOP_MEMCARD
			MemCardCreateSave(_SramPath, _MainLoop_SaveTitle, TRUE);
			#endif
		}
	}

	BOOTLOG("[boot] InitNetwork: enter\n");
	bLoadedNetwork = _MainLoopInitNetwork(ppSearchPaths);
	BOOTLOG("[boot] InitNetwork: leave (loaded=%d)\n", (int)bLoadedNetwork);

	// configure network if we started it ourselves
	if (bLoadedNetwork)
	{
		_MainLoopConfigureNetwork(_MainLoop_NetConfigPaths, (char *)"ipconfig.dat");
	}

	// load netplay module - only if the network IRX stack actually
	// came up. NETPLAY.IRX talks to PS2IP at startup, so loading it
	// without the IP stack hangs the IOP boot on emulators where
	// SMAP/PS2IP are unavailable.
	if (bLoadedNetwork)
	{
	    if (IOPLoadModule("NETPLAY.IRX", ppSearchPaths, 0, NULL) >= 0)
	    {
	        NetPlayInit((void *)_MainLoopNetCallback);
	    }
	}

    BOOTLOG("[boot] CDVD.IRX: try load\n");
    if (IOPLoadModule("CDVD.IRX", ppSearchPaths, 0, NULL) >= 0)
    {
        BOOTLOG("[boot] CDVD_Init()\n");
        CDVD_Init();
        BOOTLOG("[boot] CDVD_Init done\n");
    }
    else
    {
        BOOTLOG("[boot] CDVD.IRX skipped (not available)\n");
    }

    /* Audio: load audsrv.irx (modern PS2DEV audio service, replaces
       the legacy SjPCM stack). audsrv.irx depends on the SPU2 driver
       (sceSd*), which lives in rom0:LIBSD on most retail PS2 BIOS
       images. Early Japanese models and some emulators do not ship
       LIBSD in rom0, so we fall back to the embedded freesd.irx
       (PS2SDK's drop-in replacement at $(PS2SDK)/iop/irx/freesd.irx).
       Either of those provides the sceSd* exports audsrv binds to. */
    BOOTLOG("[boot] LIBSD/FREESD: try load\n");
    if (IOPLoadModule("rom0:LIBSD", NULL, 0, NULL) < 0)
    {
        if (IOPLoadModule("FREESD.IRX", ppSearchPaths, 0, NULL) < 0)
        {
            BOOTLOG("[boot] LIBSD/FREESD: both failed - audio will be silent\n");
        }
    }
    BOOTLOG("[boot] LIBSD/FREESD done\n");

    BOOTLOG("[boot] AUDSRV.IRX: try load\n");
    if (IOPLoadModule("AUDSRV.IRX", ppSearchPaths, 0, NULL) >= 0)
    {
        BOOTLOG("[boot] SjPCM_Init() (audsrv backend)\n");
	    if (SjPCM_Init(0, 960*25, SJPCMMIXBUFFER_MAXENQUEUE) >= 0)
	    {
	        _MainLoop_bSjPCMReady = TRUE;
	        BOOTLOG("[boot] SjPCM_Init done\n");
	    }
	    else
	    {
	        BOOTLOG("[boot] SjPCM_Init failed\n");
	    }
    }
    else
    {
        BOOTLOG("[boot] AUDSRV.IRX skipped (not available)\n");
    }

	#if 1
    BOOTLOG("[boot] MCSAVE.IRX: try load\n");
    if (IOPLoadModule("MCSAVE.IRX", ppSearchPaths, 0, NULL) >= 0)
    {
        BOOTLOG("[boot] MCSave_Init()\n");
        MCSave_Init(MAINLOOP_MAXSRAMSIZE);
        _MainLoop_bMCSaveReady = TRUE;
        BOOTLOG("[boot] MCSave_Init done\n");
    }
    else
    {
        BOOTLOG("[boot] MCSAVE.IRX skipped (not available)\n");
    }
	#endif

	if (bLoadedNetwork)
	{
		// try to load ps2link so we can have host i/o back
	    IOPLoadModule("PS2LINK.IRX", ppSearchPaths, 0, NULL);
	}
}
