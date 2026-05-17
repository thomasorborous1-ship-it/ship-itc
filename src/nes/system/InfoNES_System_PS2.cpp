/* InfoNES_System_PS2.cpp - PS2 platform layer for InfoNES.
 *
 * InfoNES_System.h declares a contract that every InfoNES platform must
 * satisfy: input poll, framebuffer flip, sound output, memcpy/memset
 * trampolines, debug print, etc. This file is the PS2 implementation.
 *
 * Phase 2 status: every callback here is a STUB. They exist so the
 * InfoNES core (InfoNES.cpp, K6502.cpp, InfoNES_pAPU.cpp,
 * InfoNES_Mapper.cpp) LINKS into the ELF. They are not actually called
 * by anything yet, because NesSystem::ExecuteFrame() does its own
 * stub rendering and never calls InfoNES_Cycle/InfoNES_Main.
 *
 * Phase 3 wires these up for real:
 *   - InfoNES_PadState()    -> read SysInputT from NesSystem::ExecuteFrame
 *   - InfoNES_LoadFrame()   -> push WorkFrame[256*240] to the
 *                              CRenderSurface passed to ExecuteFrame
 *   - InfoNES_SoundOutput() -> push samples to the SJPCM mix buffer
 *   - InfoNES_MemoryCopy/Set -> wrappers over libc memcpy/memset
 *   - InfoNES_ReadRom       -> never called; NesSystem feeds the ROM
 *                              data directly via NesHeader/ROM/VROM
 *                              globals, bypassing InfoNES_Load entirely.
 *
 * The NesPalette[] table is the 64-entry NES master palette in
 * RGB555-ish form that InfoNES uses internally. The values come from
 * upstream InfoNES (InfoNES_Sample_System.cpp from jay-kumogata's
 * InfoNES, Apache 2.0).
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "InfoNES_System.h"
#include "InfoNES_Types.h"


/* Identical to InfoNES_Sample_System.cpp, kept verbatim so any future
   re-import from upstream is a clean overwrite. */
WORD NesPalette[ 64 ] =
{
  0x39ce, 0x1071, 0x0015, 0x2013, 0x440e, 0x5402, 0x5000, 0x3c20,
  0x20a0, 0x0100, 0x0140, 0x00e2, 0x0ceb, 0x0000, 0x0000, 0x0000,
  0x5ef7, 0x01dd, 0x10fd, 0x401e, 0x5c17, 0x700b, 0x6ca0, 0x6521,
  0x45c0, 0x0240, 0x02a0, 0x0247, 0x0211, 0x0000, 0x0000, 0x0000,
  0x7fff, 0x1eff, 0x2e5f, 0x223f, 0x79ff, 0x7dd6, 0x7dcc, 0x7e67,
  0x7ae7, 0x4342, 0x2769, 0x2ff3, 0x03bb, 0x0000, 0x0000, 0x0000,
  0x7fff, 0x579f, 0x6b9f, 0x5bff, 0x7dff, 0x7ddf, 0x7e9b, 0x7ebb,
  0x7f1f, 0x5fd7, 0x575d, 0x47f3, 0x57fe, 0x0000, 0x0000, 0x0000
};


/* ---------------------------------------------------------------- */
/*  Platform callbacks - Phase 2 stubs (no behaviour).               */
/* ---------------------------------------------------------------- */

int InfoNES_Menu( void )
{
    /* InfoNES_Main() calls this in a loop. Returning -1 tells the core
       to exit gracefully. NesSystem::ExecuteFrame never enters
       InfoNES_Main, so this is dead code today. */
    return -1;
}

int InfoNES_ReadRom( const char *pszFileName )
{
    /* NesSystem will hand ROM data directly to the InfoNES globals,
       bypassing InfoNES_Load entirely. Provided only so the link
       resolves. */
    (void)pszFileName;
    return -1;
}

void InfoNES_ReleaseRom( void )
{
}

void InfoNES_LoadFrame( void )
{
    /* Phase 3: copy WorkFrame[256*240] from RGB555 to the
       CRenderSurface bound by NesSystem::ExecuteFrame. */
}

void InfoNES_PadState( DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem )
{
    if (pdwPad1)   *pdwPad1   = 0;
    if (pdwPad2)   *pdwPad2   = 0;
    if (pdwSystem) *pdwSystem = 0;
}

void *InfoNES_MemoryCopy( void *dest, const void *src, int count )
{
    return memcpy(dest, src, (size_t)count);
}

void *InfoNES_MemorySet( void *dest, int c, int count )
{
    return memset(dest, c, (size_t)count);
}

void InfoNES_DebugPrint( char *pszMsg )
{
    if (pszMsg) printf("%s", pszMsg);
}

void InfoNES_Wait( void )
{
}

void InfoNES_SoundInit( void )
{
}

int InfoNES_SoundOpen( int samples_per_sync, int sample_rate )
{
    (void)samples_per_sync;
    (void)sample_rate;
    return 1;
}

void InfoNES_SoundClose( void )
{
}

void InfoNES_SoundOutput( int samples, BYTE *wave1, BYTE *wave2,
                          BYTE *wave3, BYTE *wave4, BYTE *wave5 )
{
    (void)samples;
    (void)wave1;
    (void)wave2;
    (void)wave3;
    (void)wave4;
    (void)wave5;
}

void InfoNES_MessageBox( char *pszMsg, ... )
{
    va_list ap;
    char Buf[1024];

    va_start(ap, pszMsg);
    vsnprintf(Buf, sizeof(Buf), pszMsg, ap);
    va_end(ap);

    printf("[InfoNES] %s\n", Buf);
}
