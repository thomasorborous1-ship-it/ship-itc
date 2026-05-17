
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <kernel.h>
#include <iopheap.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <ps2_filesystem_driver.h>

#include "types.h"
#include "console.h"
#include "mainloop.h"

extern "C" {
#include "excepHandler.h"
#include "cd.h"
#include "hw.h"
};



const char *updateloader = "rom0:UDNL ";
const char *eeloadcnf = "rom0:EELOADCNF";

static char *_Main_pBootPath;
static char _Main_BootDir[256];


char *MainGetBootDir()
{
	return _Main_BootDir;
}

char *MainGetBootPath()
{
	return _Main_pBootPath;
}

void MainSetBootDir(const char *pPath)
{
	int i;
	strcpy(_Main_BootDir, pPath);

	i = strlen(_Main_BootDir);

	// search backward for start of filename
	while (i>0 
			&& _Main_BootDir[i]!='/'
			&& _Main_BootDir[i]!='\\'
			&& _Main_BootDir[i]!=':'
		) i--;

	i++;

	_Main_BootDir[i] = 0;
}

/* Reset the IOP and all of its subsystems.  */
int full_reset()
{
	char imgcmd[64];
	FILE *fp;


	/* The CDVD must be initialized here (before shutdown) or else the PS2
	   could hang on reboot.  I'm not sure why this happens.  */
	if (cdvdInit(CDVD_INIT_NOWAIT) < 0)
		return -1;

	/* Here we detect which IOP image we want to reset with.  Older Japanese
	   models don't have EELOADCNF, so we fall back on the default image
	   if necessary. rom0:EELOADCNF is served by the BIOS rom0 device,
	   which iomanX in fileXio.irx exposes to newlib stdio. */
	*imgcmd = '\0';

	if ((fp = fopen(eeloadcnf, "rb")) != NULL) {
		fclose(fp);

		strcpy(imgcmd, updateloader);
		strcat(imgcmd, eeloadcnf);
	}
//	scr_printf("rebooting with imgcmd '%s'\n", *imgcmd ? imgcmd : "(null)");

	if (cdvdInit(CDVD_EXIT) < 0)
		return -1;

//	scr_printf("Shutting down subsystems.\n");

	cdvdExit();
	deinit_ps2_filesystem_driver();
	SifExitIopHeap();
	SifLoadFileExit();
	SifExitRpc();

	SifIopReset(imgcmd, 0);
	while (!SifIopSync()) ;

	SifInitRpc(0);
	FlushCache(0);

	// initialize cdvd
//    cdvdInit(CDVD_INIT_NOWAIT);

	return 0;
}








/* Your program's main entry point */
int main(int argc, char **argv) 
{
    int iArg;

	if (argc>=1)
	{
		_Main_pBootPath = argv[0];
	}

	MainSetBootDir(_Main_pBootPath);

	printf("[SNES-AUDFIX-V3] main: enter, bootpath=%s\n", _Main_pBootPath);

	SifInitRpc(0);

	if (_Main_pBootPath[0]=='m' && _Main_pBootPath[1]=='c')
	{
//		installExceptionHandlers();

		// reset if loaded from memory card
		full_reset();
	}

	/* Patch the rom0:LOADFILE service so SifExecModuleBuffer (used by
	   our embedded-IRX loader in src/platform/ps2/system/embedded_irx.cpp)
	   and ps2_drivers' init_ps2_filesystem_driver actually work. The
	   stock retail BIOS LOADFILE module is missing LoadModuleBuffer
	   support, so without these patches the EE call "succeeds" but the
	   IRX never finishes registering its RPC server. The prefix check
	   patch additionally lets us load modules from any device, which
	   is useful for cdrom: / host: fallbacks. */
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();

	/* Bring up the modern PS2DEV filesystem stack: iomanX, fileXio,
	   poweroff, mcman/mcserv, cdfs, usb, mx4sio, dev9, hdd. Once this
	   returns, newlib stdio (fopen/fread/fwrite/fclose/mkdir/opendir)
	   routes through iomanX, so paths like "mc0:/SNESticle/<rom>.srm",
	   "cdfs:/ROMS/foo.sfc", "mass:/bar/baz" all work as standard POSIX
	   file paths from the EE side.

	   The legacy rom0:FILEIO RPC was the original I/O path in this
	   codebase (fioOpen / fioDopen / fioRead). It silently dropped a
	   non-trivial fraction of memcard reads on emulators (the SRAM
	   load bug that motivated this refactor), so we switch the whole
	   EE side over to fileXio. The fio* API stays available for callers
	   that still need it - fileXio's iomanX-based device list is a
	   superset of the legacy fileio one. */
	init_ps2_filesystem_driver();

	// initialize cdvd
    cdvdInit(CDVD_INIT_NOWAIT);

    for (iArg=0; iArg < argc; iArg++)
    {
        printf("%d: %s\n", iArg, argv[iArg]);
    }

	DmaReset();

    install_VRstart_handler();

	ConInit();

	if (MainLoopInit())
	{
		// do stuff here
		while (MainLoopProcess())
		{
		}

		MainLoopShutdown();
	}

	ConShutdown();

	return 0;
}

