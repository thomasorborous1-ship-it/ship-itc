
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

/* DLog: writes to EE SIO TX FIFO (defined in modules/sjpcm/sjpcm_rpc.c).
   Plain printf on the EE never reaches PCSX2/NetherSX2's emulator log
   in this build, so the only way to surface boot-phase diagnostics is
   via the EE SIO channel.  See sjpcm_rpc.c for the rationale. */
extern "C" void DLog(const char *fmt, ...);



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

/* Reset the IOP and all of its subsystems.

   Only reachable when booted from a memory card (see main()), which is
   not the common ELF/ISO path on emulators.  We still avoid the legacy
   custom CDVD.IRX RPC here for the same reason described next to the
   removed cdvdInit() call in main() below: the modern cdfs.irx loaded
   by ps2_drivers does not register the cdvd RPC at 0x80000592 that the
   in-tree cdvdInit() expects, so calling it spins forever in
   SifBindRpc.  All the cdfs cleanup we used to do here is now folded
   into deinit_ps2_filesystem_driver(). */
int full_reset()
{
	char imgcmd[64];
	FILE *fp;

	/* rom0:EELOADCNF is served by the BIOS rom0 device, which iomanX in
	   fileXio.irx exposes to newlib stdio.  Older Japanese models don't
	   have EELOADCNF, so we fall back on the default image if so. */
	*imgcmd = '\0';

	if ((fp = fopen(eeloadcnf, "rb")) != NULL) {
		fclose(fp);

		strcpy(imgcmd, updateloader);
		strcat(imgcmd, eeloadcnf);
	}

	deinit_ps2_filesystem_driver();
	SifExitIopHeap();
	SifLoadFileExit();
	SifExitRpc();

	SifIopReset(imgcmd, 0);
	while (!SifIopSync()) ;

	SifInitRpc(0);
	FlushCache(0);

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

	DLog("[boot] main: enter, bootpath=%s", _Main_pBootPath ? _Main_pBootPath : "(null)");

	SifInitRpc(0);
	DLog("[boot] SifInitRpc done");

	/* Reset the IOP so the BIOS-resident modules (sceCdvdfsv, sceSio2man,
	   sceMcMan, sceMcServ, etc.) are unloaded before ps2_drivers tries
	   to install its own modern copies.  Without this reset the two
	   sets of IRX modules end up half-overlapping in RPC tables and the
	   tail of init_ps2_filesystem_driver() - specifically the mcman /
	   poweroff hand-off - hangs silently after dev9 init prints its
	   banner.  This is exactly the sequence picodrive's plat.c follows
	   in platform/ps2/plat.c::reset_IOP. */
	DLog("[boot] SifIopReset: enter");
	while (!SifIopReset(NULL, 0)) {}
	while (!SifIopSync()) {}
	SifInitRpc(0);
	DLog("[boot] SifIopReset done");

	/* Patch the rom0:LOADFILE service so SifExecModuleBuffer (used by
	   our embedded-IRX loader in src/platform/ps2/system/embedded_irx.cpp)
	   and ps2_drivers' init_ps2_filesystem_driver actually work. The
	   stock retail BIOS LOADFILE module is missing LoadModuleBuffer
	   support, so without these patches the EE call "succeeds" but the
	   IRX never finishes registering its RPC server. The prefix check
	   patch additionally lets us load modules from any device, which
	   is useful for cdrom: / host: fallbacks.

	   These patches must run after SifIopReset because the reset
	   reloads rom0:LOADFILE in its pristine, unpatched state. */
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();
	DLog("[boot] sbv patches applied");

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
	DLog("[boot] init_ps2_filesystem_driver: enter");
	init_ps2_filesystem_driver();
	DLog("[boot] init_ps2_filesystem_driver: done");

	if (_Main_pBootPath[0]=='m' && _Main_pBootPath[1]=='c')
	{
		/* Reset the IOP if we were loaded from a memory card.
		   We do this AFTER init_ps2_filesystem_driver because full_reset
		   needs fopen("rom0:EELOADCNF") to work, and rom0: is only
		   routed to newlib stdio once iomanX has been brought up. */
		DLog("[boot] booted from mc -> full_reset");
		full_reset();
		DLog("[boot] full_reset done -> re-init filesystem");
		init_ps2_filesystem_driver();
		DLog("[boot] filesystem re-init done");
	}

	/* cdvdInit(CDVD_INIT_NOWAIT) used to live here.  It is intentionally
	   gone now: it binds to RPC 0x80000592 (CDVD_INIT_BIND_RPC, see
	   src/platform/ps2/cdvd/cd.c) which was served by the iaddis-era
	   custom CDVD.IRX.  That IRX is no longer loaded - the modern
	   cdfs.irx that init_ps2_filesystem_driver() loads instead exposes
	   cdfs: through iomanX and registers a different RPC number.  With
	   no server bound to 0x80000592, cdvdInit's SifBindRpc spin loop
	   never completes and the EE hangs silently (black screen, no
	   further IOP output) before MainLoopInit even gets a chance to
	   run.  All disc I/O now goes through fopen("cdfs:/...") via the
	   refactor in src/platform/ps2/system/mainloop_load.cpp etc. */

    for (iArg=0; iArg < argc; iArg++)
    {
        DLog("[boot] argv[%d] = %s", iArg, argv[iArg] ? argv[iArg] : "(null)");
    }

	DmaReset();
	DLog("[boot] DmaReset done");

    install_VRstart_handler();
    DLog("[boot] install_VRstart_handler done");

	ConInit();
	DLog("[boot] ConInit done -> MainLoopInit");

	if (MainLoopInit())
	{
		DLog("[boot] MainLoopInit OK -> entering MainLoopProcess loop");
		while (MainLoopProcess())
		{
		}

		MainLoopShutdown();
	}
	else
	{
		DLog("[boot] MainLoopInit FAILED");
	}

	ConShutdown();

	return 0;
}

