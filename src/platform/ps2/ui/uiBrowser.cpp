
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <libpad.h>
#include <stdio.h>
#include "types.h"
#define NEWLIB_PORT_AWARE
#include "fileio.h"
#if 0
#include "font.h"
#else
#include "font.h"
#endif
#include "poly.h"
#include "uiBrowser.h"
extern "C" {
#include "cdvd_rpc.h"
#include "mcsave_ee.h"
#include "ps2mem.h"	/* PS2MEM_UNCACHED */
};

static const char *_MenuEntries[]=
{
	"Copy File",
	"Paste File",
	"Delete file",
	NULL
};

int CBrowserScreen::GetEntryPath(char *pStr, int nChars)
{
	if (m_iSelect >=0 && m_iSelect < m_nEntries)
		return snprintf(pStr, nChars, "%s%s", m_Dir, m_pDirEntries[m_iSelect].name);
	else 
		return 0;
}

char *CBrowserScreen::GetEntryName()
{
	if (m_iSelect >=0 && m_iSelect < m_nEntries)
		return m_pDirEntries[m_iSelect].name;
	else 
		return NULL;
}

BrowserEntryTypeE CBrowserScreen::GetEntryType()
{
	if (m_iSelect >=0 && m_iSelect < m_nEntries)
		return m_pDirEntries[m_iSelect].eType;
	else 
		return BROWSER_ENTRYTYPE_OTHER;
}

typedef int (*CopyProgressCallBackT)(char *pDestName, char *pSrcName, int Position, int Total);
int CopyFile(char *pDest, char *pSrc, CopyProgressCallBackT pCallBack);
int PathGetMaxFileNameLength(const char *pPath);
void PathTruncFileName(Char *pOut, Char *pStr, Int32 nMaxChars);

int CBrowserScreen::MenuEvent(Uint32 Type, Uint32 Parm1, void *Parm2)
{
	CBrowserScreen *pBrowser = (CBrowserScreen *)Parm2;
	Char str[256];

	if (pBrowser->GetEntryPath(str, sizeof(str)) == 0)
	{
		return 0;
	}

	switch (Type)
	{
		case 1:
			switch (Parm1)
			{
				case 0: // copy file
					switch (pBrowser->GetEntryType())
					{
						case BROWSER_ENTRYTYPE_DRIVE:
							break;
						case BROWSER_ENTRYTYPE_DIR:
							break;
						default:
							pBrowser->m_SubMenu.SetText(0, str);
							pBrowser->m_SubMenu.SetText(1, pBrowser->GetEntryName());
							break;
					}
					break;
				case 1: // Paste file
					{
						char strDestPath[1024];
						char strSrcPath[1024];
						char strDestShortName[256];
						char strDestFileName[256];
						char strDestFileExt[256];
						char *pExt;

						// get dest file name
						strcpy(strDestFileName, pBrowser->m_SubMenu.GetText(1));
						strDestFileExt[0] = '\0';

						// split dest file name by extension
						pExt = strrchr(strDestFileName, '.');
						if (pExt)
						{
							// special case .gz extensions							
							if (!strcmp(pExt, ".gz"))
							{
								*pExt = '\0';
								pExt = strrchr(strDestFileName, '.');
								if (pExt)
								{
									strcpy(strDestFileExt, pExt);
									*pExt = '\0';
								}
								strcat(strDestFileExt, ".gz");

							} else
							{
								strcpy(strDestFileExt, pExt);
								*pExt = '\0';
							}
						}
						// truncate file name
						PathTruncFileName(strDestShortName, strDestFileName, PathGetMaxFileNameLength(pBrowser->m_Dir) - strlen(strDestFileExt));
						
						snprintf(strDestPath, sizeof(strDestPath), "%s%s%s", pBrowser->m_Dir, strDestShortName, strDestFileExt);
						snprintf(strSrcPath, sizeof(strSrcPath), "%s", pBrowser->m_SubMenu.GetText(0));


						printf("src: %s\n", strSrcPath );
						printf("dest: %s\n", strDestPath);
						CopyFile(strDestPath, strSrcPath, NULL);

						pBrowser->Chdir(".");
					}
					break;
				case 2: // delete file

			        printf("Deleting %s...\n", str);
					switch (pBrowser->GetEntryType())
					{
						case BROWSER_ENTRYTYPE_DRIVE:
							break;
						case BROWSER_ENTRYTYPE_DIR:
							fioRmdir(str);
							break;
						default:
							fioRemove(str);
							fioRmdir(str);
							break;
					}
					pBrowser->Chdir(".");

					break;
			}

			pBrowser->m_bSubMenu = FALSE;

		break;
	}

	return 0;
}


CBrowserScreen::CBrowserScreen(Uint32 uMaxEntries)
{
	m_Dir[0]=0;
	m_nEntries=0;
	m_MaxEntries = uMaxEntries;
	m_iSelect=0;
	m_iScroll=0;
	m_MaxLines = (209 / 11 - 1); // umm, hacked
	m_bMCDir = FALSE;
	m_bSubMenu = FALSE;
	m_pDirEntries = new BrowserEntryT[uMaxEntries];

	m_SubMenu.SetTitle("File Menu");
	m_SubMenu.SetEntries((char **)_MenuEntries);
	m_SubMenu.SetMsgFunc(MenuEvent);
	m_SubMenu.SetUserData(this);
}

CBrowserScreen::~CBrowserScreen()
{
	delete m_pDirEntries;
}

void CBrowserScreen::ResetEntries()
{
	m_iSelect  = 0;
	m_nEntries = 0;
	m_iScroll  = 0;
}



static Int32 _BrowserEntryQSort(const void *pA, const void *pB)
{
	BrowserEntryT *pDirA, *pDirB;
	pDirA = (BrowserEntryT *)pA;
	pDirB = (BrowserEntryT *)pB;

	if (pDirA->eType == pDirB->eType)
	{
		return strcasecmp(pDirA->name, pDirB->name);
	} 
	else
	{
		return pDirA->eType - pDirB->eType;
	}	
}

void CBrowserScreen::SortEntries()
{
	qsort(m_pDirEntries, m_nEntries, sizeof(m_pDirEntries[0]), _BrowserEntryQSort);
}


void CBrowserScreen::AddEntry(const Char *pName, BrowserEntryTypeE eType, Int32 size)
{
	if (m_nEntries < m_MaxEntries)
	{
		strncpy(m_pDirEntries[m_nEntries].name, pName, BROWSER_ENTRY_MAXCHARS - 1);
		m_pDirEntries[m_nEntries].name[BROWSER_ENTRY_MAXCHARS-1] = '\0';
		m_pDirEntries[m_nEntries].size = size;
		m_pDirEntries[m_nEntries].eType = eType;
		m_nEntries++;
	}
}


void CBrowserScreen::Draw()
{
	Int32 iEntry;
	Int32 vx=4, vy = 20;
	Int32 iLine;

	iEntry = m_iScroll;

	FontSelect(0);

	PolyTexture(NULL);
    PolyBlend(TRUE);


//    PolyColor4f(0.0f, 0.2f, 0.2f, 0.5f); 
    PolyColor4f(0.0f, 0.2f, 0.2f, 0.9f); 
	PolyRect(0, vy, 256, 9);

	FontColor4f(0.0, 0.8f, 0.8f, 1.0f);
    FontPrintf(vx, vy, "%s", m_Dir);
    vy+=12;

	for (iLine=0; iLine < m_MaxLines; iLine++)
	{
		Char str[128];
		Char sizestr[128];

		if (iEntry>=0 && iEntry < m_nEntries)
		{
			BrowserEntryT *pEntry = &m_pDirEntries[iEntry];
			if (pEntry->eType==BROWSER_ENTRYTYPE_DIR)
			{
				sprintf(str, "/%s", pEntry->name);
				sprintf(sizestr, " ");
			}
			else
			if (pEntry->eType==BROWSER_ENTRYTYPE_DRIVE)
			{
				sprintf(str, "%s", pEntry->name);
				sprintf(sizestr, " ");
			}
			else
			{
				sprintf(str, "%s", pEntry->name);
				sprintf(sizestr, "%3dK", pEntry->size / 1024);
			}

			// limit length of string
			str[60] = 0;

			// render selection bar
			if (iEntry == m_iSelect)
			{
				if (iEntry == m_iSelect)
					PolyColor4f(0.0f, 1.0f, 0.0f, 0.5f); 
					else
					PolyColor4f(0.0f, 0.0f, 0.0f, 0.25f); 

				PolyRect(vx-1, vy-1, FontGetStrWidth(str) + 2, FontGetHeight() + 2);
//				PolyRect(vx-2, vy-0, strlen(str) * 12 + 2, 13 + 0);
			}

			// render menu entry
			switch(pEntry->eType)
			{
				case BROWSER_ENTRYTYPE_DRIVE:
					FontColor4f(0.0, 0.8f, 0.8f, 1.0f);
					break;
				case BROWSER_ENTRYTYPE_DIR:
					FontColor4f(1.0, 1.0f, 0.0f, 1.0f);
					break;
				case BROWSER_ENTRYTYPE_OTHER:
					FontColor4f(0.25, 0.25f, 0.25f, 1.0f);
					break;
                default:
					FontColor4f(0.8, 0.8f, 0.8f, 1.0f);
					break;
			}

		   //			FontColor4f(0.8, 0.8f, 0.8f, 1.0f);

			FontPuts(vx, vy, str);
//			FontPuts(vx+480, vy, sizestr);
		}

		vy += FontGetHeight() + 2;
		iEntry++;
	}


/*
	FontSelect(0);
	FontColor4f(0.5, 0.5f, 0.5f, 1.0f);
	FontPuts(10, 220, "Select=Network");
  */

/*
	FontPuts(0, 210, "+ - \"");


    PolyBlend(TRUE);
    PolyTexture(&_FontTexture);
    PolyUV(0,0,256,32);
	PolyColor4f(1.0, 1.0, 1.0, 1.0f);
    PolyRect(0,120,256,32);
*/



	FontSelect(0);

	if (m_bSubMenu)
	{
		PolyTexture(NULL);
		PolyBlend(TRUE);
		PolyColor4f(0,0,0,0.8f);
		PolyRect(0, 0, 256, 224);

		m_SubMenu.Draw();
	}
}

void CBrowserScreen::Process()
{
}


void CBrowserScreen::Input(Uint32 buttons, Uint32 trigger)
{
	if (trigger & PAD_SELECT)
	{
		m_bSubMenu = !m_bSubMenu;
		  /*
		if (m_bSubMenu)
		{
	    	Char str[256];

//	        sprintf(str, "%s%s", m_Dir, m_pDirEntries[m_iSelect].name);
	        sprintf(str, "%s", m_pDirEntries[m_iSelect].name);
			m_SubMenu.SetTitle(str);
		}
		*/
	}

	if (m_bSubMenu)
	{
		m_SubMenu.Input(buttons,trigger);
		return;
	}

	if (trigger & PAD_UP)
	{
		m_iSelect--;
	}

	if (trigger & PAD_DOWN)
	{
		m_iSelect++;
	}

	if (trigger & (PAD_SQUARE))
	{
		m_iSelect-= m_MaxLines-1;
	}

	if (trigger & (PAD_CIRCLE))
	{
		m_iSelect+= m_MaxLines-1;
	}

	// scroll
	if (m_iSelect < 0) m_iSelect = 0;
 	if (m_iSelect > (m_nEntries - 1)) m_iSelect = (m_nEntries - 1);

	// scroll
	if (m_iSelect < m_iScroll)
	{
		m_iScroll = m_iSelect;
	}

	if (m_iSelect >= (m_iScroll + m_MaxLines - 1))
	{
		m_iScroll = m_iSelect - m_MaxLines + 1;
	}

	if (trigger & PAD_TRIANGLE)
    {
        Chdir("..");
    }

	if (trigger & (PAD_CROSS | PAD_START))
	{
		char str[256];

		if (GetEntryPath(str, sizeof(str))!=0)
		{

			CDVD_FlushCache();

	        switch(m_pDirEntries[m_iSelect].eType)
	        {
	        case BROWSER_ENTRYTYPE_DIR:
	        case BROWSER_ENTRYTYPE_DRIVE:
				Chdir(m_pDirEntries[m_iSelect].name);
	            break;

	        default:
		        printf("exec: %s\n", str);
				SendMessage(1, m_pDirEntries[m_iSelect].eType, (void *)str);
	            break;
	            
			}
		}
		return;
	}



}


#ifdef _EE
static int _BrowserDread(int fd, io_dirent_t *dirent, Bool bIsMCDir)
#else
static int _BrowserDread(int fd, fio_dirent_t *dirent, Bool bIsMCDir)
#endif
{
	/* MCSave_Dread is only meaningful for memcard directories: it
	   forwards the fd to MCSAVE.IRX, which has its own private
	   universe of fds. Sending a `cdfs:` (or `host:`/`mass:`) fd
	   through it returns garbage and the directory looks empty.
	   For non-memcard paths fall back to the regular fioDread. */
	/* Defensive zeroing: the iaddis CDVD.IRX has a memset bug in
	   CDVD_dread (memset(dirent, 0, sizeof(dirent)) only clears
	   4 bytes - sizeof of a pointer - so most of the stat struct
	   carries values from the previous entry). Wipe the buffer
	   on the EE side first so any field the IRX leaves alone is
	   guaranteed to be 0 instead of stale. */
#ifdef _EE
	memset(dirent, 0, sizeof(io_dirent_t));
#else
	memset(dirent, 0, sizeof(fio_dirent_t));
#endif

	if (bIsMCDir && MCSave_IsInitialized())
	{
		return MCSave_Dread(fd, dirent);
	}
	return fioDread(fd, dirent);
}


void CBrowserScreen::SetDir(const Char *pDir)
{
//    Int32 nEntries, iEntry;
	int fd;

    printf("MenuDir: %s\n", pDir);

	ResetEntries();

	strcpy(m_Dir, pDir);
	/* "mc0:/..." or "mc1:/..." -> route Dread through MCSave_Dread.
	   Anything else (cdfs:, host:, mass:, ...) goes via fioDread. */
	m_bMCDir = (pDir[0] == 'm' && pDir[1] == 'c' && pDir[3] == ':');
	m_iScroll = 0;
	m_iSelect = 0;

	ForceDraw();


	if (strlen(pDir) > 0)
	{
		fd = fioDopen(pDir);
		if (fd >= 0)
		{
			/* Access dirbuf through the uncached EE segment
			   (KSEG1 mirror, +0x20000000) so reads of the dread
			   buffer always come from physical memory - never from
			   the EE's stale data cache.

			   Background: fioDread / MCSave_Dread call
			   SifWriteBackDCache(buf, ...) before the RPC, but
			   neither calls SifInvalidateDCache afterwards. The IOP
			   then writes the directory entry into main memory via
			   SBUS DMA, but the EE's L1 D$ has no automatic snoop
			   for SBUS DMA, so dirent fields that are still resident
			   in cache from a previous iteration (or from the EE's
			   own pre-RPC memset) get read back instead of the
			   freshly DMAed bytes. In practice this was making
			   stat.attr carry the SUBDIR bit (0x10) of an earlier
			   directory entry into a regular file slot, so a couple
			   of files (e.g. MCSAVE.IRX, NETPLAY.IRX) were rendered
			   with the directory colour even though the ISO 9660
			   directory bit was clearly off. Bypassing the cache via
			   PS2MEM_UNCACHED removes the coherency hazard. */
			static Uint8 dirbuf[512] __attribute__((aligned(64)));
#ifdef _EE
			io_dirent_t *dirent =
				(io_dirent_t *)PS2MEM_UNCACHED(&dirbuf);
#else
			fio_dirent_t *dirent = (fio_dirent_t *)&dirbuf;
#endif
		 //	printf("fioDopen: %s %d %d %d\n", pDir, fd, sizeof(dirent), sizeof(fio_dirent_t));
			
//			while (fioDread(fd, dirent) > 0) // && m_nEntries < 1280)

			while (_BrowserDread(fd, dirent, m_bMCDir) > 0) // && m_nEntries < 1280)
		    {
		        BrowserEntryTypeE eType;
										  
		   //     printf("%s %02X %d \n", 		            dirent->name, dirent->stat.attr,    dirent->stat.size		            );
				if (strcmp((char *)dirent->name,".") && strcmp((char *)dirent->name,".."))
				{
			        /* Modern PS2SDK no longer ships FIO_ATTR_SUBDIR (it lived in
			           the legacy fileio.h that came with iaddis SNESticle), so the
			           original entry-type detection used to live behind `#if 0`,
			           which left eType uninitialised and made the renderer paint
			           every entry as BROWSER_ENTRYTYPE_DIR (yellow). The two IRX
			           drivers we actually use - the iaddis CDVD.IRX for cdfs: and
			           MCSAVE.IRX for mc0:/mc1: - both flag directories the same
			           way: bit 0x10 in stat.attr (the legacy FIO_ATTR_SUBDIR value;
			           cdvd_iop.c line ~742 sets it directly from the ISO 9660
			           directory bit). Recheck that bit explicitly so the legacy
			           IRX behaviour matches even though the SDK header dropped the
			           macro. The stat.mode field is left untouched by both
			           IRX drivers (they only write addr/attr/size/hisize),
			           so we cannot rely on FIO_S_IFDIR there. */
			        const unsigned int kLegacyAttrSubdir = 0x10;
			        bool bIsDir = (dirent->stat.attr & kLegacyAttrSubdir) != 0;

			        if (bIsDir)
			        {
			            eType = BROWSER_ENTRYTYPE_DIR;
			        } else
			        {
			            /* Resolve known ROM extensions to EXECUTABLE, leave
			               everything else as OTHER (rendered dim). */
			            eType = (BrowserEntryTypeE)SendMessage(2, 0, (void *)dirent->name);
			            if (eType != BROWSER_ENTRYTYPE_EXECUTABLE)
			                eType = BROWSER_ENTRYTYPE_OTHER;
			        }
			        AddEntry((char *)dirent->name, eType, dirent->stat.size);
					/*
					if (!(m_nEntries & 127))
						ForceDraw();
						*/
				}
		    }
				
		 	// printf("fioClose: %s %d\n", pDir, fd);
			fioDclose(fd);
		}
	} else
	{
        AddEntry("cdfs:", BROWSER_ENTRYTYPE_DRIVE, 0);
//        AddEntry("cdrom:", BROWSER_ENTRYTYPE_DRIVE, 0);
        AddEntry("host:", BROWSER_ENTRYTYPE_DRIVE, 0);
        AddEntry("mc0:", BROWSER_ENTRYTYPE_DRIVE, 0);
        AddEntry("mc1:", BROWSER_ENTRYTYPE_DRIVE, 0);
//        AddEntry("rom:", BROWSER_ENTRYTYPE_DRIVE, 0);
	}

	SortEntries();

    printf("BrowserEntries: %d\n", m_nEntries);
}

void CBrowserScreen::Chdir(const Char *pSubDir)
{
	Char dir[256];

	strcpy(dir, m_Dir);

	if (!strcmp(pSubDir, "."))
	{
	} else
	if (!strcmp(pSubDir, ".."))
	{
        if (strcmp(dir,"/"))
        {
		    Int32 i = strlen(dir) - 2;

		    // backup
		    while (i >= 0 && dir[i]!='/')
		    {
			    i--;
		    }
            i++;

		    dir[i] = 0;
        }
	}
	else
	{
		strcat(dir, pSubDir);
		strcat(dir, "/");
	}

	SetDir(dir);
}


