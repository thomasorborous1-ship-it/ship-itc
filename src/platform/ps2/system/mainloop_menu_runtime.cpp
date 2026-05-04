/* mainloop_menu_runtime.cpp
 *
 * Hosts the runtime menu helpers used by MainLoopRender() and the input
 * path:
 *
 *   - _MenuEnable() : toggle the in-game menu, flushing SRAM to memcard
 *                     when the menu is brought up.
 *   - _MenuDraw()   : per-frame menu overlay (called from
 *                     MainLoopRender()).
 *
 * _MenuDraw() used to be a file-static helper inside mainloop.cpp.
 * After the Batch 3 split it has external linkage so MainLoopRender()
 * (now in mainloop_render.cpp) can still reach it through the
 * declaration in mainloop_shared.h.
 *
 * Extracted from mainloop.cpp during the Batch 3 split. Behaviour and
 * the surrounding `#if MAINLOOP_MEMCARD` / `#if 0` gating are unchanged.
 */

#include <stdio.h>
#include <string.h>

#include "mainloop_debug.h"
#include "mainloop_shared.h"
#include "mainloop_state.h"
#include "mainloop_ui.h"

#include "types.h"
#include "console.h"
#include "font.h"
#include "poly.h"
#include "memcard.h"
#include "uiScreen.h"

extern "C" {
#include "ps2ip.h"
};

extern "C" {
#include "mcsave_ee.h"
};


/* MAINLOOP_MEMCARD lives in mainloop_shared.h (included above) and
   gates the memcard SRAM / save-file path inside _MenuEnable(). */


void _MenuEnable(Bool bEnable)
{
	if (bEnable!=_bMenu)
	{
		// if menu is enabled, then attempt to save sram immediately
		if (bEnable)
		{
            #if 1 
			if (_MainLoopHasSRAM() && _MainLoop_SRAMUpdated)
			{
			   	MainLoopModalPrintf(10, "Saving SRAM...");

			    if (_MainLoopHasSRAM())
			    {
					#if MAINLOOP_MEMCARD
					if (_MainLoop_bMCSaveReady) MCSave_WriteSync(1, NULL);

					if (MemCardCheckNewCard())
					{
						printf("New memcard detected\n");
						if (MemCardCreateSave(_SramPath, _MainLoop_SaveTitle, FALSE))
						{
							MemCardCreateSave(_SramPath, _MainLoop_SaveTitle, FALSE);
						}
					}
					#endif

				    if (_MainLoopSaveSRAM(TRUE))
				    {
					    MainLoopModalPrintf(60, "SRAM saved.\n");
				    } else
				    {
					    MainLoopModalPrintf(60 * 1 + 30, "Error Saving SRAM!\n");
				    }
			    }
			}
            #endif
		}

		_bMenu = bEnable;

//    	SjPCM_Clearbuff();
	}
}




void _MenuDraw()
{
	FontSelect(0);

	PolyTexture(NULL);
    PolyBlend(TRUE);


    t_ip_info config;
    memset(&config, 0, sizeof(config));

	// draw current screen
	if (_MainLoop_pScreen)
	{
		_MainLoop_pScreen->Draw();
	}

	int vy = 215;

	FontSelect(2);
//	FontColor4f(1.0, 0.0f, 0.0f, 1.0f);
//	FontColor4f(1.0, 0.5f, 0.5f, 1.0f);
	FontColor4f(0.2, 0.6f, 0.2f, 1.0f);

#if 0
	const VersionInfoT *pVersionInfo = VersionGetInfo();

	char VersionStr[256];
	
	sprintf(VersionStr, "%s v%d.%d.%d %s", 
		pVersionInfo->ApplicationName, 
		pVersionInfo->Version[0],
		pVersionInfo->Version[1],
		pVersionInfo->Version[2],
		pVersionInfo->BuildType
		);

	FontPuts(256 - 16 - FontGetStrWidth(VersionStr), vy, VersionStr);

//	FontPrintf(8, vy-16, "%d", CDVD_DiskReady(1));




	FontPrintf(8, vy, "%s%d.%d", 
		pVersionInfo->Compiler, 
		pVersionInfo->CompilerVersion[0],  
		pVersionInfo->CompilerVersion[1]
		);
#endif	
    FontPrintf(48,vy,"IP: %d.%d.%d.%d", 
            (config.ipaddr.s_addr >> 0) & 0xFF,
            (config.ipaddr.s_addr >> 8) & 0xFF,
            (config.ipaddr.s_addr >>16) & 0xFF,
            (config.ipaddr.s_addr >>24) & 0xFF
                    );



	FontSelect(0);
}
