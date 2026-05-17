
#include <kernel.h>
#include <libmc.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "types.h"

#include "memcard.h"

static Uint8 _MemCard_IconData[]={
#include "memcard_icon.inc"
};

static Bool _MemCard_bInitialized=FALSE;

int MemCardCreateSave(char *pDir, char *pTitle, Bool bForceWrite)
{
	int icon_size;
	char* icon_buffer;
	mcIcon icon_sys;
	char Path[256];
	int mkRet;

	static iconIVECTOR bgcolor[4] = {
		{  68,  23, 116,  0 },
		{ 255, 255, 255,  0 },
		{ 255, 255, 255,  0 },
		{  68,  23, 116,  0 },
	};

	static iconFVECTOR lightdir[3] = {
		{ 0.5, 0.5, 0.5, 0.0 },
		{ 0.0,-0.4,-0.1, 0.0 },
		{-0.5,-0.5, 0.5, 0.0 },
	};

	static iconFVECTOR lightcol[3] = {
		{ 0.3, 0.3, 0.3, 0.00 },
		{ 0.4, 0.4, 0.4, 0.00 },
		{ 0.5, 0.5, 0.5, 0.00 },
	};

	static iconFVECTOR ambient = { 0.50, 0.50, 0.50, 0.00 };

	printf("MemCard: CreateSave('%s', '%s', force=%d) init=%d\n",
	       pDir, pTitle, (int)bForceWrite, (int)_MemCard_bInitialized);

	if (!_MemCard_bInitialized)
	{
		printf("MemCard: CreateSave bail (mc not initialized)\n");
		return -1;
	}

	/* mkdir() goes through iomanX -> mcman, which accepts paths of
	   the form "mc0:/SNESticle". On a fresh card the directory does
	   not exist and mkdir returns 0. On an already-populated card it
	   returns -1 with errno=EEXIST, which is fine: the existing files
	   will be overwritten below. */
	mkRet = mkdir(pDir, 0777);
	printf("MemCard: mkdir('%s') -> %d (errno=%d)\n",
	       pDir, mkRet, mkRet < 0 ? errno : 0);

	if (mkRet < 0 && errno != EEXIST)
	{
		if (!bForceWrite)
		{
			printf("MemCard: CreateSave bail (mkdir failed, no force)\n");
		 	return -1;
		}
	}

	memset(&icon_sys, 0, sizeof(mcIcon));
	strcpy((char *)icon_sys.head, "PS2D");
#ifdef _EE
	strcpy((char*)&icon_sys.title, (const char*)pTitle);
#else
	strcpy_sjis((short *)&icon_sys.title, pTitle);
#endif
	icon_sys.nlOffset = 16;
	icon_sys.trans = 0x60;
	memcpy(icon_sys.bgCol, bgcolor, sizeof(bgcolor));
	memcpy(icon_sys.lightDir, lightdir, sizeof(lightdir));
	memcpy(icon_sys.lightCol, lightcol, sizeof(lightcol));
	memcpy(icon_sys.lightAmbient, ambient, sizeof(ambient));
	strcpy((char *)icon_sys.view, "icon.icn");
	strcpy((char *)icon_sys.copy, "icon.icn");
	strcpy((char *)icon_sys.del, "icon.icn");

	sprintf(Path, "%s/icon.sys", pDir);
	if (!MemCardWriteFile(Path, (Uint8 *)&icon_sys, sizeof(icon_sys)))
	{
		printf("MemCard: icon.sys write failed\n");
		return -5;
	}

	icon_size = sizeof(_MemCard_IconData);
	icon_buffer = (char *)_MemCard_IconData;

	sprintf(Path, "%s/icon.icn", pDir);
	if (!MemCardWriteFile(Path, (Uint8 *)icon_buffer, icon_size))
	{
		printf("MemCard: icon.icn write failed\n");
		return -6;
	}
	printf("MemCard: CreateSave OK\n");
	return 0;
}

Bool MemCardCheckNewCard()
{
	int mc_Type, mc_Free, mc_Format;
	int ret;

	if (!_MemCard_bInitialized) return FALSE;

	mcGetInfo(0,0,&mc_Type,&mc_Free,&mc_Format);
	mcSync(0, NULL, &ret);
	printf("MemCard: mcGetInfo -> ret=%d type=%d free=%d fmt=%d\n",
	       ret, mc_Type, mc_Free, mc_Format);

	if (ret == -1)
	{
		mcGetInfo(0,0,&mc_Type,&mc_Free,&mc_Format);
		mcSync(0, NULL, &ret);
		return TRUE;
	}
	return FALSE;
}

void MemCardInit()
{
	int rc = mcInit(MC_TYPE_MC);
	printf("MemCard: mcInit -> %d\n", rc);
	if (rc < 0) {
		printf("MemCard: Failed to initialise memcard server!\n");
	} else
	{
		printf("MemCard: Initialized\n");
		_MemCard_bInitialized = TRUE;
	}
}

/* All memcard I/O now goes through newlib stdio (fopen/fread/fwrite/
   fclose). Once init_ps2_filesystem_driver() has run, mc paths of the
   form "mc0:/SNESticle/<rom>.srm" are handled by iomanX -> mcman.irx
   directly, which is the same path picodrive / OPL / uLE use and is
   much more reliable than the legacy rom0:FILEIO RPC the project
   used before (fioOpen / fioRead).

   In particular, with newlib stdio the SRAM read path actually
   returns the full requested byte count on the first try, instead of
   the partial / zero-byte reads we saw with fioRead on real hardware
   - which was the root cause of "save works but load does not". */

Bool MemCardWriteFile(char *pPath, Uint8 *pData, Uint32 nBytes)
{
	FILE *fp;
	size_t result;

	if (!_MemCard_bInitialized)
	{
		printf("MemCard: Write skipped (not init): %s\n", pPath);
		return FALSE;
	}

	fp = fopen(pPath, "wb");
	printf("MemCard: fopen-W('%s') -> %p\n", pPath, fp);
	if (!fp)
	{
		printf("MemCard: Write FAIL open: %s (errno=%d)\n", pPath, errno);
		return FALSE;
	}

	result = fwrite(pData, 1, nBytes, fp);
	fflush(fp);
	fclose(fp);
	printf("MemCard: fwrite('%s') %u/%u%s\n",
	       pPath, (unsigned)result, (unsigned)nBytes,
	       (result == nBytes) ? "" : " <<< MISMATCH");
	return (result == nBytes);
}

Bool MemCardReadFile(char *pPath, Uint8 *pData, Uint32 nBytes)
{
	FILE *fp;
	size_t result;

	printf("MemCard: ReadFile('%s', %u) init=%d\n",
	       pPath, (unsigned)nBytes, (int)_MemCard_bInitialized);

	if (!_MemCard_bInitialized)
	{
		printf("MemCard: Read skipped (not init): %s\n", pPath);
		return FALSE;
	}

	fp = fopen(pPath, "rb");
	printf("MemCard: fopen-R('%s') -> %p\n", pPath, fp);
	if (!fp)
	{
		printf("MemCard: Read FAIL open: %s (errno=%d)\n", pPath, errno);
		return FALSE;
	}

	result = fread(pData, 1, nBytes, fp);
	fclose(fp);
	printf("MemCard: fread('%s') %u/%u%s\n",
	       pPath, (unsigned)result, (unsigned)nBytes,
	       (result == nBytes) ? "" : " <<< MISMATCH");
	return (result == nBytes);
}
