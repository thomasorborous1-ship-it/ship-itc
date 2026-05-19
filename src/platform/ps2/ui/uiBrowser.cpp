
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <libpad.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include "types.h"
#if 0
#include "font.h"
#else
#include "font.h"
#endif
#include "poly.h"
#include "uiBrowser.h"

extern "C" void DLog(const char *fmt, ...);

extern "C" {
#include "mcsave_ee.h"
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
	/* m_Dir is 512 chars and an entry name can now be up to 255; pick
	   1024 so the joined path can never wrap on us regardless of how
	   deep the user has nested their ROM library. */
	Char str[1024];

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
							rmdir(str);
							break;
						default:
							unlink(str);
							rmdir(str);
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
		/* sized to match BROWSER_ENTRY_MAXCHARS (256) so snprintf with
		   "%s" + pEntry->name cannot trip -Wformat-truncation. The
		   visible portion is still capped to 120 chars at the end of
		   this block so we don't overrun the on-screen list column. */
		Char str[BROWSER_ENTRY_MAXCHARS + 4];
		Char sizestr[32];

		if (iEntry>=0 && iEntry < m_nEntries)
		{
			BrowserEntryT *pEntry = &m_pDirEntries[iEntry];
			if (pEntry->eType==BROWSER_ENTRYTYPE_DIR)
			{
				snprintf(str, sizeof(str), "/%s", pEntry->name);
				sprintf(sizestr, " ");
			}
			else
			if (pEntry->eType==BROWSER_ENTRYTYPE_DRIVE)
			{
				snprintf(str, sizeof(str), "%s", pEntry->name);
				sprintf(sizestr, " ");
			}
			else
			{
				snprintf(str, sizeof(str), "%s", pEntry->name);
				sprintf(sizestr, "%3dK", pEntry->size / 1024);
			}

			/* The original iaddis truncation hard-stopped names at 60
			   chars to keep them inside the FontPuts viewport. With
			   BROWSER_ENTRY_MAXCHARS bumped to 256 the displayed string
			   no longer has to be the same width as the storage buffer,
			   so we keep a visual cap to avoid clipping into the right
			   panel but raise it from 60 -> 120 so users can actually
			   see enough of long No-Intro / GoodSNES filenames to pick
			   the right ROM. The 60-char cut never affected fopen since
			   GetEntryPath reads from m_pDirEntries[i].name directly. */
			str[120] = 0;

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
		/* Same sizing rationale as in MenuEvent: m_Dir up to 512 + entry
		   name up to 255 fits comfortably in 1024, and this is the path
		   that eventually reaches fopen() through _MainLoopExecuteFile. */
		char str[1024];

		if (GetEntryPath(str, sizeof(str))!=0)
		{

			/* Modern cdfs.irx handles cache invalidation internally on
			   directory re-open; the legacy CDVD_FlushCache() RPC is no
			   longer needed (and the IRX it talked to is no longer
			   loaded). */

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


/* Directory iteration via newlib stdio + dirent.h. opendir/readdir
   route through iomanX once init_ps2_filesystem_driver has run,
   so cdfs:/, mc0:/, mass:/, host:/ all use the same API path.

   We can no longer rely on dirent->d_type alone to tell files apart
   from subdirectories - cdfs.irx leaves it at DT_UNKNOWN, and a
   subset of older iomanX backends also under-fill it. To stay robust
   across every device we always confirm directories with a follow-up
   stat() on the joined path. The same trick handles the legacy
   CDVD.IRX bug where a stray SUBDIR bit leaked into regular files
   (see PR #76 for the original symptom). */

void CBrowserScreen::SetDir(const Char *pDir)
{
    DIR *dir;

    DLog("[ui] MenuDir: '%s'", pDir);

	ResetEntries();

	strcpy(m_Dir, pDir);
	/* Kept for legacy callers that read m_bMCDir; with iomanX the
	   directory iteration path is the same for every device. */
	m_bMCDir = (pDir[0] == 'm' && pDir[1] == 'c' && pDir[3] == ':');
	m_iScroll = 0;
	m_iSelect = 0;

	ForceDraw();


	if (strlen(pDir) > 0)
	{
		dir = opendir(pDir);
		DLog("[ui] opendir('%s') -> %p (errno=%d)", pDir, (void *)dir, dir ? 0 : errno);
		if (dir != NULL)
		{
			struct dirent *de;
			while ((de = readdir(dir)) != NULL)
			{
				BrowserEntryTypeE eType;
				Char childPath[1024];
				struct stat st;
				bool bIsDir = false;
				Int32 nSize = 0;

				if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
					continue;

				/* Trust d_type only when it is concrete; otherwise stat
				   the joined path. cdfs.irx leaves d_type=DT_UNKNOWN. */
				bool typeKnown = false;
#ifdef DT_DIR
				if (de->d_type == DT_DIR) { bIsDir = true; typeKnown = true; }
				else if (de->d_type == DT_REG) { typeKnown = true; }
#endif
				if (!typeKnown)
				{
					snprintf(childPath, sizeof(childPath),
					         "%s%s", m_Dir, de->d_name);
					if (stat(childPath, &st) == 0)
					{
						bIsDir = S_ISDIR(st.st_mode) ? true : false;
						nSize  = (Int32)st.st_size;
					}
				}

				if (bIsDir)
				{
					eType = BROWSER_ENTRYTYPE_DIR;
				}
				else
				{
					eType = (BrowserEntryTypeE)SendMessage(
						2, 0, (void *)de->d_name);
					if (eType != BROWSER_ENTRYTYPE_EXECUTABLE)
						eType = BROWSER_ENTRYTYPE_OTHER;

					/* Pick up size if we did not stat above. */
					if (nSize == 0)
					{
						snprintf(childPath, sizeof(childPath),
						         "%s%s", m_Dir, de->d_name);
						if (stat(childPath, &st) == 0)
							nSize = (Int32)st.st_size;
					}
				}

				AddEntry(de->d_name, eType, nSize);
			}
			closedir(dir);
		}
	} else
	{
        AddEntry("cdfs:", BROWSER_ENTRYTYPE_DRIVE, 0);
//        AddEntry("cdrom:", BROWSER_ENTRYTYPE_DRIVE, 0);
        AddEntry("host:", BROWSER_ENTRYTYPE_DRIVE, 0);
        AddEntry("mass:", BROWSER_ENTRYTYPE_DRIVE, 0);
        AddEntry("mc0:", BROWSER_ENTRYTYPE_DRIVE, 0);
        AddEntry("mc1:", BROWSER_ENTRYTYPE_DRIVE, 0);
//        AddEntry("rom:", BROWSER_ENTRYTYPE_DRIVE, 0);
	}

	SortEntries();

    DLog("[ui] BrowserEntries: %d (dir='%s')", m_nEntries, m_Dir);
}

void CBrowserScreen::Chdir(const Char *pSubDir)
{
	/* m_Dir is 512 and pSubDir can be a 255-char entry name; pick 1024
	   so the strcat below cannot overflow even on the deepest nested
	   ROM directories. */
	Char dir[1024];

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


