
#include <stdio.h>
#include <string.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <kernel.h>
#define NEWLIB_PORT_AWARE
#include <fileio.h>
#include <iopheap.h>
#include <iopcontrol.h>
#include <debug.h>
#include "platform/ps2/system/boot_status.h"

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
	int fd;


	/* The CDVD must be initialized here (before shutdown) or else the PS2
	   could hang on reboot.  I'm not sure why this happens.  */
	if (cdvdInit(CDVD_INIT_NOWAIT) < 0)
		return -1;

	/* Here we detect which IOP image we want to reset with.  Older Japanese
	   models don't have EELOADCNF, so we fall back on the default image
	   if necessary.  */
	*imgcmd = '\0';

	if ((fd = fioOpen(eeloadcnf, O_RDONLY)) >= 0) {
		fioClose(fd);

		strcpy(imgcmd, updateloader);
		strcat(imgcmd, eeloadcnf);
	}
//	scr_printf("rebooting with imgcmd '%s'\n", *imgcmd ? imgcmd : "(null)");

	if (cdvdInit(CDVD_EXIT) < 0)
		return -1;

//	scr_printf("Shutting down subsystems.\n");

	cdvdExit();
	fioExit();
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
#if DEBUG_BOOT_SCREEN
	/* init_scr() configures the GS for the BIOS debug font and writes
	   directly to the framebuffer with no IOP/SIF/host: round-trip,
	   so it works on emulators where stdout host:tty: doesn't exist.
	   Keep this GS configuration alive for the entire boot when
	   DEBUG_BOOT_SCREEN is set so every scr_printf() further down is
	   visible on screen even if some later init step hangs. The app's
	   normal rendering path (GS_InitGraph + GS_SetEnv in MainLoopInit)
	   is bypassed in this mode - the goal is to find the hang, not to
	   render the menu. */
	init_scr();
	scr_setbgcolor(0x00000000);
	scr_setfontcolor(0x00FFFFFF);
	scr_clear();
	scr_setCursor(1);
	BootStatusLog("[boot] main: argc=%d argv0=%s", argc, (argc >= 1 && argv[0]) ? argv[0] : "<null>");
#endif

	if (argc>=1)
	{
		_Main_pBootPath = argv[0];
	}

	MainSetBootDir(_Main_pBootPath);

#if DEBUG_BOOT_SCREEN
	BootStatusLog("[boot] SifInitRpc...");
#endif
	SifInitRpc(0);
#if DEBUG_BOOT_SCREEN
	BootStatusLog("[boot] SifInitRpc OK");
#endif

	if (_Main_pBootPath[0]=='m' && _Main_pBootPath[1]=='c')
	{
//		installExceptionHandlers();

		// reset if loaded from memory card
		full_reset();
	}

	// initialize cdvd
#if DEBUG_BOOT_SCREEN
	BootStatusLog("[boot] cdvdInit(NOWAIT)...");
#endif
    cdvdInit(CDVD_INIT_NOWAIT);
#if DEBUG_BOOT_SCREEN
	BootStatusLog("[boot] cdvdInit OK");
#endif

    for (iArg=0; iArg < argc; iArg++)
    {
        printf("%d: %s\n", iArg, argv[iArg]);
    }

	DmaReset();
#if DEBUG_BOOT_SCREEN
	BootStatusLog("[boot] DmaReset OK");
#endif

    install_VRstart_handler();
#if DEBUG_BOOT_SCREEN
	BootStatusLog("[boot] install_VRstart_handler OK");
#endif

	ConInit();
#if DEBUG_BOOT_SCREEN
	BootStatusLog("[boot] ConInit OK");
	BootStatusLog("[boot] -> MainLoopInit() ...");
#endif

	if (MainLoopInit())
	{
#if DEBUG_BOOT_SCREEN
		BootStatusLog("[boot] MainLoopInit returned TRUE");
		BootStatusLog("[boot] -> MainLoopProcess loop");
#endif
		// do stuff here
		while (MainLoopProcess())
		{
		}

		MainLoopShutdown();
	}
#if DEBUG_BOOT_SCREEN
	else
	{
		BootStatusLog("[boot] MainLoopInit returned FALSE!");
		/* Stay on screen so the user can read the trace. */
		while (1) { }
	}
#endif

	ConShutdown();

	return 0;
}

