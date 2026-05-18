
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
#define NEWLIB_PORT_AWARE
#include <fileXio.h>
#include <fileXio_rpc.h>
#undef NEWLIB_PORT_AWARE
#include <ps2_filesystem_driver.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <libcdvd.h>

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
	   poweroff, mcman/mcserv, cdfs, usb.  Once this is done, newlib
	   stdio (fopen/fread/fwrite/fclose/mkdir/opendir) routes through
	   iomanX, so paths like "mc0:/SNESticle/<rom>.srm",
	   "cdfs:/ROMS/foo.sfc", "mass:/bar/baz" all work as standard POSIX
	   file paths from the EE side.

	   The legacy rom0:FILEIO RPC was the original I/O path in this
	   codebase (fioOpen / fioDopen / fioRead).  It silently dropped a
	   non-trivial fraction of memcard reads on emulators (the SRAM
	   load bug that motivated this refactor), so we switch the whole
	   EE side over to fileXio.  The fio* API stays available for callers
	   that still need it - fileXio's iomanX-based device list is a
	   superset of the legacy fileio one.

	   We deliberately do NOT call the all-in-one
	   init_ps2_filesystem_driver() that ps2_drivers ships.  That
	   helper also calls init_dev9_driver(), init_hdd_driver(),
	   mount_current_hdd_partition() and waitUntilDeviceIsReady(cwd) at
	   the end, all of which we don't need (SNESticle never touches the
	   PS2 HDD or DEV9 hardware) and at least one of which hangs
	   silently after dev9 prints "unknown dev9 hardware" on emulators
	   and most retail PS2s.  Inlining the bring-up here lets us bracket
	   every step with a DLog so the next hang, if any, can be pinpointed
	   directly from the EE_SIO emulator log. */
	DLog("[boot] init_poweroff_driver: enter");
	init_poweroff_driver();
	DLog("[boot] init_poweroff_driver: done");

	DLog("[boot] init_fileXio_driver: enter");
	init_fileXio_driver();
	DLog("[boot] init_fileXio_driver: done");

	DLog("[boot] init_memcard_driver: enter");
	init_memcard_driver(true);
	DLog("[boot] init_memcard_driver: done");

	DLog("[boot] init_usb_driver: enter");
	init_usb_driver();
	DLog("[boot] init_usb_driver: done");

	DLog("[boot] init_cdfs_driver: enter");
	init_cdfs_driver();
	DLog("[boot] init_cdfs_driver: done");

	/* Kick the IOP-side cdvdman so sceCdGetDiskType returns the real
	   disc type instead of SCECdNODISC.  Without this cdfs.irx's
	   isValidDisc refuses to enumerate the root, and
	   opendir("cdfs:/") returns NULL - which is exactly the
	   "browser shows no files" symptom.  Same call the working
	   InfinityStation project uses in
	   ps2boot/storage/disc.c::ps2_disc_init_once. */
	DLog("[boot] sceCdInit: enter");
	sceCdInit(SCECdINIT);
	DLog("[boot] sceCdInit: done (diskType=%d)", sceCdGetDiskType());

	/* Runtime FS probe: log opendir/stat for every top-level mount so
	   the next boot tells us exactly where the browser breaks. The
	   browser uses printf which never reaches the SIO log; this dup
	   via DLog does. */
	DLog("[probe] fs probe begin");
	{
		const char *paths[] = { "cdfs:/", "cdfs:", "mc0:/", "mc0:", "mass:/", "host:/" };
		int i;
		for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
			DIR *d; struct stat st; int rc;
			errno = 0; rc = stat(paths[i], &st);
			DLog("[probe] stat('%s') -> %d (errno=%d, mode=0%o)",
			     paths[i], rc, errno, rc == 0 ? (unsigned)st.st_mode : 0);
			errno = 0; d = opendir(paths[i]);
			DLog("[probe] opendir('%s') -> %p (errno=%d)", paths[i], (void *)d, errno);
			if (d) {
				struct dirent *de; int n = 0;
				while ((de = readdir(d)) != NULL && n < 8) {
					DLog("[probe]   readdir[%d] = '%s'", n, de->d_name); n++;
				}
				DLog("[probe]   total entries listed = %d", n);
				closedir(d);
			}
		}
	}
	DLog("[probe] fs probe end");

	/* Direct fileXio probe: bypass newlib entirely. If these work
	   where opendir() above does not, the EE newlib<->iomanX glue
	   is the issue and the browser should call fileXio* directly. */
	DLog("[fxprobe] direct fileXio probe begin");
	{
		const char *paths[] = { "cdfs:/", "cdfs:", "mc0:/", "mass:/", "host:/" };
		int i;
		struct fileXioDirEntry de;
		iox_stat_t st;
		for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
			int sr = fileXioGetStat(paths[i], &st);
			DLog("[fxprobe] fileXioGetStat('%s') -> %d (mode=0x%x)",
			     paths[i], sr, sr == 0 ? (unsigned)st.mode : 0);
			int d = fileXioDopen(paths[i]);
			DLog("[fxprobe] fileXioDopen('%s') -> %d", paths[i], d);
			if (d >= 0) {
				int n = 0;
				while (fileXioDread(d, &de) > 0 && n < 8) {
					DLog("[fxprobe]   dread[%d] = '%s' (mode=0x%x)", n, de.name, (unsigned)de.stat.mode);
					n++;
				}
				DLog("[fxprobe]   total entries listed = %d", n);
				fileXioDclose(d);
			}
		}
	}
	DLog("[fxprobe] direct fileXio probe end");

	if (_Main_pBootPath[0]=='m' && _Main_pBootPath[1]=='c')
	{
		/* Reset the IOP if we were loaded from a memory card.
		   We do this AFTER the filesystem stack is up because full_reset
		   needs fopen("rom0:EELOADCNF") to work, and rom0: is only
		   routed to newlib stdio once iomanX has been brought up. */
		DLog("[boot] booted from mc -> full_reset");
		full_reset();
		DLog("[boot] full_reset done -> re-init filesystem");
		init_poweroff_driver();
		init_fileXio_driver();
		init_memcard_driver(true);
		init_usb_driver();
		init_cdfs_driver();
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

