/* mainloop_globals.cpp
 *
 * Single home for the *definitions* of every cross-file symbol that
 * the rest of the mainloop_*.cpp tree reaches for through
 * mainloop_shared.h.
 *
 * No logic lives here -- only object definitions. Anything with
 * file-static linkage stays inside the .cpp that uses it (e.g.
 * mainloop_init.cpp, mainloop_process.cpp), and #if 0 dead code from
 * the original mainloop.cpp stays in mainloop.cpp.
 *
 * Extracted from mainloop.cpp during the Batch 3 split. No values,
 * initialisers, or attribute lists changed.
 */

#include "types.h"
#include "mainloop_shared.h"

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


/* MAINLOOP_MEMCARD lives in mainloop_shared.h (included above) and
   gates the memcard variants of _SramPath / _MainLoop_SaveTitle below. */


/* ---- UI screens --------------------------------------------------- */

CBrowserScreen *_MainLoop_pBrowserScreen;
CNetworkScreen *_MainLoop_pNetworkScreen;
CMenuScreen    *_MainLoop_pMenuScreen;
CLogScreen     *_MainLoop_pLogScreen;
CScreen        *_MainLoop_pScreen = NULL;


/* ---- Emulator core handles ---------------------------------------- */

SnesSystem *_pSnes;
SnesRom    *_pSnesRom;

#if 0
static NesSystem  *_pNes;
static NesRom	  *_pNesRom;
static NesFDSBios  *_pNesFDSBios;
static NesDisk	  *_pNesFDSDisk;
static Int32 _MainLoop_iDisk=0;
static Bool _MainLoop_bDiskInserted=FALSE;
#endif

Char _RomName[256];

#if MAINLOOP_MEMCARD
Char _SramPath[256] = "mc0:/SNESticle";
Char _MainLoop_SaveTitle[] = "SNESticle\nSNESticle";
#else
Char _SramPath[256] = "host0:/cygdrive/d/emu/";
#endif

Emu::System *_pSystem;


/* ---- ROM / framebuffer / audio buffers ---------------------------- */

CRenderSurface *_fbTexture[2];

TextureT _OutTex;
Uint32 _MainLoop_uOutTexTBP  = 0;
Uint32 _MainLoop_uBlenderTBP = 0;
#ifdef DEBUG
CWavFile _WavFile;
#endif

Uint8 _RomData[4 * 1024 * 1024 + 1024] __attribute__((aligned(64))) __attribute__ ((section (".bss")));

SnesStateT		_SnesState;
#if 0
static NesStateT		_NesState;
#endif

Emu::MovieClip *s_pMovieClip;


/* ---- SRAM / save bookkeeping -------------------------------------- */

Uint32 _MainLoop_SRAMChecksum;
Uint32 _MainLoop_SaveCounter   = 0;
Uint32 _MainLoop_AutoSaveTime  = 8 * 60;
Bool   _MainLoop_SRAMUpdated   = FALSE;
Bool   _bStateSaved            = FALSE;
Float32 _MainLoop_fOutputIntensity = 0.8f;

SJPCMMixBuffer *_SJPCMMix;


/* ---- Flags / counters --------------------------------------------- */

Bool _bMenu = FALSE;

Char  _MainLoop_ModalStr[256];
Int32 _MainLoop_ModalCount = 0;

Char  _MainLoop_StatusStr[256];
Int32 _MainLoop_StatusCount = 0;

Bool   _MainLoop_BlackScreen   = FALSE;
Uint32 _MainLoop_uDebugDisplay = 0;

Uint32 _uInputFrame;
Uint32 _uInputChecksum[5];
