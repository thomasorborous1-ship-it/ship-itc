
#include <kernel.h>
#include <libmc.h>
#include <stdio.h>
#include <string.h>
#define NEWLIB_PORT_AWARE
#include <fileio.h>
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

	mkRet = fioMkdir(pDir);
	printf("MemCard: fioMkdir('%s') -> %d\n", pDir, mkRet);

	if (mkRet < 0)
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

Bool MemCardWriteFile(char *pPath, Uint8 *pData, Uint32 nBytes)
{
	int fd;

	if (!_MemCard_bInitialized)
	{
		printf("MemCard: Write skipped (not init): %s\n", pPath);
		return FALSE;
	}

	fd = fioOpen(pPath, O_WRONLY | O_CREAT);
	printf("MemCard: fioOpen-W('%s') -> %d\n", pPath, fd);
	if (fd > 0)
	{
		unsigned int result;
		result = fioWrite(fd, pData, nBytes);
		fioClose(fd);
		printf("MemCard: fioWrite('%s') %u/%u\n", pPath, result, (unsigned)nBytes);
		return (result == nBytes);
	}
	return FALSE;
}

Bool MemCardReadFile(char *pPath, Uint8 *pData, Uint32 nBytes)
{
	int fd;

	if (!_MemCard_bInitialized) return FALSE;

	fd = fioOpen(pPath, O_RDONLY);
	if (fd > 0)
	{
		unsigned int result;
		result = fioRead(fd, pData, nBytes);
		fioClose(fd);
		printf("MemCard: Read %s (%d)\n", pPath, result);
		return (result == nBytes);
	}
	return FALSE;
}
