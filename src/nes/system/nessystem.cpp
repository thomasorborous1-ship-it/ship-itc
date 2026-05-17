/* nessystem.cpp - NesSystem implementation (Phase 2 STUB).
 *
 * The InfoNES core itself is NOT yet wired up here. This file exists so
 * that the rest of the mainloop can hold an Emu::System* that "is a"
 * NesSystem*, dispatch to ExecuteFrame() through the polymorphic base,
 * and not crash when the user picks a .nes from the browser.
 *
 * What we do paint in Phase 2: a recognisable diagnostic test pattern
 * (a moving band) onto pTarget so the user gets immediate visual
 * confirmation that the dispatch reached our code. Pure black would be
 * indistinguishable from a crash.
 *
 * Phase 3 will replace ExecuteFrame() with a real InfoNES_Cycle() call
 * plus a converter from the 256x240 16-bpp InfoNES WorkFrame[] to the
 * SnesPPU-style render surface the rest of mainloop_process.cpp feeds
 * to TextureUpload(). The SetRom() path will be expanded then too -
 * it will hand the .nes bytes to InfoNES, configure NesHeader / ROM /
 * VROM, call the per-mapper init, and invoke InfoNES_Reset().
 */

#include <stdio.h>
#include <string.h>

#include "nessystem.h"
#include "rendersurface.h"
#include "surface.h"
#include "pixelformat.h"


NesSystem::NesSystem()
{
    m_pNesRom    = NULL;
    m_pNesDisk   = NULL;
    m_uFrameTick = 0;
    m_uFrame     = 0;
    m_uLine      = 0;
}

NesSystem::~NesSystem()
{
}


void NesSystem::SetRom(Emu::Rom *pRom)
{
    /* mainloop_load.cpp passes one of:
         - NULL                  (unload between titles)
         - NesRom*               (.nes cart)
         - NesDisk*              (.fds disk)
         - NesFDSBios*           (FDS BIOS, when pBios path is taken)
       Since RTTI is disabled (-fno-rtti) we cannot dynamic_cast, but
       the mainloop_load.cpp switch already classified the file by
       extension, so we just record the base pointer here and let
       Phase 3/5 use it. */
    m_pNesRom  = NULL;
    m_pNesDisk = NULL;
    (void)pRom;
}


void NesSystem::Reset()
{
    m_uFrameTick = 0;
    m_uFrame     = 0;
    m_uLine      = 0;
    /* Phase 3: InfoNES_Init(); InfoNES_Reset(); per-mapper init. */
}

void NesSystem::SoftReset()
{
    Reset();
}


void NesSystem::ExecuteFrame(Emu::SysInputT *pInput,
                             CRenderSurface *pTarget,
                             CMixBuffer *pMixBuf,
                             ModeE eMode)
{
    (void)pInput;
    (void)pMixBuf;
    (void)eMode;

    if (!pTarget)
    {
        m_uFrame++;
        return;
    }

    /* Clear first, then write a moving horizontal band as a "are we
       alive" signal. The band sweeps down the surface so a frozen
       screen on the NetherSX2 OSD is distinguishable from a working
       (but boring) one. */
    pTarget->Clear();

    Uint32 uWidth  = pTarget->GetWidth();
    Uint32 uHeight = pTarget->GetHeight();

    /* The output texture used by the rest of the mainloop is 256x256
       RGBA8 (mainloop_init.cpp:270). Each line is 256*4 = 1024 bytes
       laid out R,G,B,A. The visible NES area is 256x240. */
    PixelFormatT *pFmt = pTarget->GetFormat();
    if (!pFmt || pFmt->uBitDepth != 32)
    {
        m_uFrame++;
        return;
    }

    Uint32 uBandRow = (m_uFrameTick / 2) % uHeight;
    for (Uint32 iY = 0; iY < uHeight; iY++)
    {
        Uint8 *pLine = pTarget->GetLinePtr((Int32)iY);
        if (!pLine) continue;

        /* base: dark blue (so "NES selected" is unambiguously NOT
           a black-screen crash). */
        for (Uint32 iX = 0; iX < uWidth; iX++)
        {
            pLine[iX * 4 + 0] = 0x10; /* R */
            pLine[iX * 4 + 1] = 0x20; /* G */
            pLine[iX * 4 + 2] = 0x80; /* B */
            pLine[iX * 4 + 3] = 0xFF; /* A */
        }

        /* a bright horizontal band that sweeps top-to-bottom every
           ~2 seconds (uHeight * 2 frames). Confirms the loop is
           ticking, not just static. */
        if (iY == uBandRow)
        {
            for (Uint32 iX = 0; iX < uWidth; iX++)
            {
                pLine[iX * 4 + 0] = 0xFF;
                pLine[iX * 4 + 1] = 0xC0;
                pLine[iX * 4 + 2] = 0x10;
                pLine[iX * 4 + 3] = 0xFF;
            }
        }
    }

    m_uFrameTick++;
    m_uFrame++;
}


/* ------------- State + SRAM stubs (real impl in Phases 3/5) ----- */

Int32 NesSystem::GetStateSize()
{
    return (Int32)sizeof(NesStateT);
}

void NesSystem::SaveState(void *pState, Int32 nStateBytes)
{
    if (!pState || nStateBytes <= 0) return;
    memset(pState, 0, (size_t)nStateBytes);
}

void NesSystem::RestoreState(void *pState, Int32 nStateBytes)
{
    (void)pState;
    (void)nStateBytes;
}

void NesSystem::SaveState(NesStateT *pState)
{
    if (!pState) return;
    memset(pState, 0, sizeof(*pState));
}

Bool NesSystem::RestoreState(NesStateT *pState)
{
    (void)pState;
    return FALSE;
}

Int32 NesSystem::GetSRAMBytes()
{
    /* Phase 5: probe NesRom + InfoNES_Reset to learn the real size. */
    return 0;
}

Uint8 *NesSystem::GetSRAMData()
{
    return NULL;
}

const char *NesSystem::GetString(StringE eString)
{
    switch (eString)
    {
        case STRING_SHORTNAME: return "NES";
        case STRING_FULLNAME:  return "Nintendo Entertainment System";
        case STRING_SRAMEXT:   return "srm";
        case STRING_STATEEXT:  return "nst";
    }
    return "";
}

Uint32 NesSystem::GetSampleRate()
{
    /* InfoNES_pAPU mixes at 22050 Hz on the existing ports. */
    return 22050;
}
