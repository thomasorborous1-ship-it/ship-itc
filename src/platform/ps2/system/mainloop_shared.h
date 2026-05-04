#pragma once

/* Shared declarations for the PS2 main loop translation units.
 *
 * Every global that lives in mainloop.cpp (or any mainloop_*.cpp that
 * other mainloop_*.cpp files reach for) is declared here ONCE, so the
 * sibling files can just `#include "mainloop_shared.h"` instead of
 * each one carrying its own bag of inline `extern` redeclarations.
 *
 * Definitions stay where they are today (mostly mainloop.cpp); this
 * header only changes how those symbols are *visible* to the rest of
 * the main-loop tree. No runtime behaviour change.
 */

#include "types.h"
#include "snes.h"
#include "snstate.h"
#include "snrom.h"
#include "emusys.h"
#include "emumovie.h"
#include "rendersurface.h"
#include "texture.h"
#include "sjpcmbuffer.h"
#include "wavfile.h"
#include "uiBrowser.h"
#include "uiNetwork.h"
#include "uiMenu.h"
#include "uiLog.h"
#include "uiScreen.h"


/* ---- Strings / paths ---------------------------------------------- */

extern Char _RomName[256];
extern Char _SramPath[256];
extern Char _MainLoop_SaveTitle[];
extern Char _MainLoop_ModalStr[256];
extern Char _MainLoop_StatusStr[256];

/* ---- Emulator core handles ---------------------------------------- */

extern Emu::System    *_pSystem;
extern Emu::MovieClip *s_pMovieClip;
extern SnesSystem     *_pSnes;
extern SnesRom        *_pSnesRom;
extern SnesStateT      _SnesState;

/* ---- ROM / framebuffer / audio buffers ---------------------------- */

extern Uint8           _RomData[4 * 1024 * 1024 + 1024];
extern CRenderSurface *_fbTexture[2];
extern TextureT        _OutTex;
extern SJPCMMixBuffer *_SJPCMMix;
#ifdef DEBUG
extern CWavFile _WavFile;
#endif

/* ---- UI screens ---------------------------------------------------- */

extern CBrowserScreen *_MainLoop_pBrowserScreen;
extern CNetworkScreen *_MainLoop_pNetworkScreen;
extern CMenuScreen    *_MainLoop_pMenuScreen;
extern CLogScreen     *_MainLoop_pLogScreen;
extern CScreen        *_MainLoop_pScreen;

/* ---- Flags / counters --------------------------------------------- */

extern Bool    _bMenu;
extern Bool    _bStateSaved;
extern Bool    _MainLoop_BlackScreen;
extern Int32   _MainLoop_ModalCount;
extern Int32   _MainLoop_StatusCount;
extern Uint32  _MainLoop_uDebugDisplay;
extern Uint32  _uInputFrame;
extern Uint32  _uInputChecksum[5];

/* ---- SRAM / save bookkeeping -------------------------------------- */

extern Uint32  _MainLoop_SRAMChecksum;
extern Uint32  _MainLoop_SaveCounter;
extern Uint32  _MainLoop_AutoSaveTime;
extern Bool    _MainLoop_SRAMUpdated;
extern Float32 _MainLoop_fOutputIntensity;

/* ---- Function entrypoints across mainloop_*.cpp ------------------- */

void MainLoopRender();
void _MenuEnable(Bool bEnable);
/* Drawn from MainLoopRender() (mainloop_render.cpp), defined in
   mainloop_menu_runtime.cpp. Was a file-static helper inside
   mainloop.cpp; promoted to extern when MainLoopRender() and the
   menu-runtime were split into separate translation units. */
void _MenuDraw();


enum
{
	MAINLOOP_ENTRYTYPE_GZ          ,
	MAINLOOP_ENTRYTYPE_ZIP         ,
	MAINLOOP_ENTRYTYPE_NESROM      ,
	MAINLOOP_ENTRYTYPE_NESFDSDISK  ,
	MAINLOOP_ENTRYTYPE_NESFDSBIOS  ,
	MAINLOOP_ENTRYTYPE_SNESROM     ,
	MAINLOOP_ENTRYTYPE_SNESPALETTE ,

	MAINLOOP_ENTRYTYPE_NUM
};
