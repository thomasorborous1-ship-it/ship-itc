/* InfoNES_System_PS2.cpp - PS2 platform layer for InfoNES.
 *
 * InfoNES_System.h declares a contract that every InfoNES platform must
 * satisfy: input poll, framebuffer flip, sound output, memcpy/memset
 * trampolines, debug print, etc. This file is the PS2 implementation
 * plus a one-frame stepper used by NesSystem::ExecuteFrame.
 *
 * Phase 3 status:
 *   - InfoNES_PadState  - reads g_pNesInputState (SNES bit layout) and
 *                         remaps to NES PAD1_Latch / PAD2_Latch.
 *   - InfoNES_LoadFrame - converts WorkFrame[256*240] (RGB555) into
 *                         g_pNesTargetSurface (RGBA8 256x256).
 *   - InfoNES_RunOneFrame - inlined InfoNES_Cycle body, bounded to a
 *                         single NES frame (262 scanlines).  Called by
 *                         NesSystem::ExecuteFrame.
 *   - InfoNES_MemoryCopy / MemorySet - libc trampolines.
 *   - InfoNES_DebugPrint / MessageBox - printf.
 *   - InfoNES_Sound*    - still no-op stubs (Phase 4).
 *
 * The NesPalette[] table is the 64-entry NES master palette in RGB555
 * form that InfoNES uses internally. Values from upstream InfoNES.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "types.h"

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_Types.h"
#include "K6502.h"

#include "emuinput.h"     /* Emu::SysInputT */
#include "rendersurface.h"
#include "pixelformat.h"

/* SNES bit layout we need to translate FROM.  _MainLoopInput is still
   wired to _MainLoopSnesInput at this point so every connected pad in
   SysInputT.uPad[i] uses these bits regardless of which emulator is
   running. Phase 4 will route per-system. */
#include "snio.h"


/* Per-frame state owned by nessystem.cpp; we just read it.

   InfoNES is compiled as C++ (.cpp files), so plain `extern` is enough
   here - everything links with C++ linkage. We deliberately don't use
   `extern "C"` because the InfoNES headers themselves don't, and any
   linkage mismatch on these globals would silently break at link time. */
extern CRenderSurface       *g_pNesTargetSurface;
extern Emu::SysInputT       *g_pNesInputState;

/* SpriteJustHit lives in InfoNES.cpp but isn't externed by InfoNES.h.
   Declare it here so InfoNES_RunOneFrame can mirror the sprite-0 hit
   timing exactly the way InfoNES_Cycle does. */
extern int SpriteJustHit;


/* ---- NES master palette, RGB555 (verbatim InfoNES upstream) ------- */
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


/* ------------------------------------------------------------------ *
 *  InfoNES_RunOneFrame                                                *
 * ------------------------------------------------------------------ *
 * NesSystem::ExecuteFrame calls this to advance the emulator by
 * exactly one NES frame.  It's a stripped-down copy of InfoNES_Cycle
 * from InfoNES.cpp - same instruction stream + HSync calls - but
 * bounded by scanline count so it returns at end-of-frame instead of
 * looping forever waiting for a PAD_SYS_QUIT.
 *
 * One frame = 263 scanlines (SCAN_VBLANK_END is 262 inclusive).  At
 * scanline 240 InfoNES_HSync calls InfoNES_LoadFrame, which writes the
 * fully rendered WorkFrame[] into the target surface.  At scanline
 * 243 (SCAN_VBLANK_START) InfoNES_HSync calls InfoNES_PadState; we
 * also handle NMI on VBlank there.
 *
 * If InfoNES_HSync ever returns -1 (PAD_SYS_QUIT) we break early.
 * Our PadState never sets QUIT so this is just a safety net.
 */
void InfoNES_RunOneFrame(void)
{
    /* One NES frame.  PPU_Scanline wraps from SCAN_VBLANK_END (262)
       back to 0 inside InfoNES_HSync; we just need to step enough
       scanlines that we land back at the start of the next frame. */
    for (int sl = 0; sl < 263; sl++)
    {
        int nStep;

        if (SpriteJustHit == PPU_Scanline &&
            PPU_ScanTable[PPU_Scanline] == SCAN_ON_SCREEN)
        {
            /* Sprite-0 hit needs the CPU to be advanced to the correct
               X position within the scanline before R2_HIT_SP fires.
               STEP_PER_SCANLINE is 113 PPU dots; SPR_X = sprite-0 X. */
            nStep = SPRRAM[SPR_X] * STEP_PER_SCANLINE / NES_DISP_WIDTH;
            K6502_Step((WORD)nStep);

            if ((PPU_R1 & R1_SHOW_SP) && (PPU_R1 & R1_SHOW_SCR))
                PPU_R2 |= R2_HIT_SP;

            if ((PPU_R0 & R0_NMI_SP) && (PPU_R1 & R1_SHOW_SP))
                NMI_REQ;

            K6502_Step((WORD)(STEP_PER_SCANLINE - nStep));
        }
        else
        {
            K6502_Step((WORD)STEP_PER_SCANLINE);
        }

        /* Frame IRQ counter tick (matches InfoNES.cpp:629-635). */
        FrameStep += STEP_PER_SCANLINE;
        if (FrameStep > STEP_PER_FRAME && FrameIRQ_Enable)
        {
            FrameStep %= STEP_PER_FRAME;
            IRQ_REQ;
            APU_Reg[0x4015] |= 0x40;
        }

        /* Per-mapper hsync callback. */
        MapperHSync();

        /* Standard InfoNES per-scanline housekeeping (also draws the
           visible scanline, polls input at VBlank, etc.). */
        if (InfoNES_HSync() == -1)
            break;
    }
}


/* ------------------------------------------------------------------ *
 *  InfoNES_LoadFrame                                                  *
 * ------------------------------------------------------------------ *
 * Called once per visible NES frame from inside InfoNES_HSync (at
 * SCAN_UNKNOWN_START, after every scanline 0..239 has been rendered
 * by InfoNES_DrawLine).  WorkFrame[256*240] is in RGB555 (5 bits per
 * channel, LSB = blue, MSB = unused) thanks to NesPalette being in
 * that format and PalTable[] mirroring it.
 *
 * Target is a 256x256 RGBA8 surface (mainloop_init.cpp:274 allocates
 * _fbTexture[] as PIXELFORMAT_RGBA8).  We write the NES visible 240
 * lines and leave 16 padding lines below as black (they're outside
 * the on-screen quad in mainloop_render.cpp anyway).
 *
 * Bit layout:
 *   RGB555: 0 RRRRR GGGGG BBBBB
 *   RGBA8:  RR GG BB AA (little-endian: R is first byte at offset 0).
 */
void InfoNES_LoadFrame(void)
{
    CRenderSurface *pTarget = g_pNesTargetSurface;
    if (!pTarget) return;

    PixelFormatT *pFmt = pTarget->GetFormat();
    if (!pFmt || pFmt->uBitDepth != 32) return;

    Uint32 uWidth  = pTarget->GetWidth();
    Uint32 uHeight = pTarget->GetHeight();
    if (uWidth < NES_DISP_WIDTH || uHeight < NES_DISP_HEIGHT) return;

    /* Convert 240 NES lines into the top 240 rows of the texture. */
    for (Uint32 iY = 0; iY < NES_DISP_HEIGHT; iY++)
    {
        Uint8 *pDst = pTarget->GetLinePtr((Int32)iY);
        if (!pDst) continue;

        WORD *pSrc = &WorkFrame[iY * NES_DISP_WIDTH];

        for (Uint32 iX = 0; iX < NES_DISP_WIDTH; iX++)
        {
            WORD w = pSrc[iX];

            /* RGB555 -> RGBA8.  Expand 5 bits to 8 by left-shifting
               and OR'ing the top 3 bits back in so 0x1F maps to 0xFF
               instead of 0xF8. */
            Uint8 r5 = (Uint8)((w >> 10) & 0x1F);
            Uint8 g5 = (Uint8)((w >>  5) & 0x1F);
            Uint8 b5 = (Uint8)( w        & 0x1F);

            pDst[iX * 4 + 0] = (Uint8)((r5 << 3) | (r5 >> 2));
            pDst[iX * 4 + 1] = (Uint8)((g5 << 3) | (g5 >> 2));
            pDst[iX * 4 + 2] = (Uint8)((b5 << 3) | (b5 >> 2));
            pDst[iX * 4 + 3] = 0xFF;
        }
    }

    /* Black out the 16 padding rows below the NES image (texture is
       256 high, NES is 240).  Without this they'd hold stale pixels
       from whatever ran last (SNES PPU, menu surface, etc.). */
    for (Uint32 iY = NES_DISP_HEIGHT; iY < uHeight; iY++)
    {
        Uint8 *pDst = pTarget->GetLinePtr((Int32)iY);
        if (pDst) memset(pDst, 0, uWidth * 4);
    }
}


/* ------------------------------------------------------------------ *
 *  InfoNES_PadState                                                   *
 * ------------------------------------------------------------------ *
 * Polled once per frame at SCAN_VBLANK_START from InfoNES_HSync.
 * We read g_pNesInputState (SNES bit layout) and remap into NES
 * controller bits.
 *
 * Standard NES bit order (lowest first, matches the serial protocol
 * that K6502_rw.h::PAD1 reads bit-by-bit):
 *
 *   bit 0 = A         bit 4 = UP
 *   bit 1 = B         bit 5 = DOWN
 *   bit 2 = SELECT    bit 6 = LEFT
 *   bit 3 = START     bit 7 = RIGHT
 *
 * Player mapping (matches how _MainLoopSnesInput already turned the
 * PS2 buttons into SNES bits):
 *   PS2 Circle / Triangle (= SNES A / SNES X) -> NES A
 *   PS2 Cross  / Square   (= SNES B / SNES Y) -> NES B
 *   PS2 Select / Start                         -> NES Select / Start
 *
 * PAD_System is for emulator-level commands like PAD_SYS_QUIT; we
 * never set it so InfoNES_HSync never breaks out of InfoNES_Cycle on
 * QUIT.  Menu return is handled by _MainLoopInputProcess instead.
 */
static DWORD MapSnesToNes(Uint16 snes)
{
    DWORD nes = 0;
    if (snes == EMUSYS_DEVICE_DISCONNECTED) return 0;

    if (snes & (SNESIO_JOY_A | SNESIO_JOY_X))      nes |= 0x01; /* A      */
    if (snes & (SNESIO_JOY_B | SNESIO_JOY_Y))      nes |= 0x02; /* B      */
    if (snes &  SNESIO_JOY_SELECT)                  nes |= 0x04; /* SELECT */
    if (snes &  SNESIO_JOY_START)                   nes |= 0x08; /* START  */
    if (snes &  SNESIO_JOY_UP)                      nes |= 0x10;
    if (snes &  SNESIO_JOY_DOWN)                    nes |= 0x20;
    if (snes &  SNESIO_JOY_LEFT)                    nes |= 0x40;
    if (snes &  SNESIO_JOY_RIGHT)                   nes |= 0x80;
    return nes;
}

void InfoNES_PadState( DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem )
{
    Emu::SysInputT *pInput = g_pNesInputState;
    DWORD p1 = 0, p2 = 0;

    if (pInput)
    {
        p1 = MapSnesToNes(pInput->uPad[0]);
        p2 = MapSnesToNes(pInput->uPad[1]);
    }

    if (pdwPad1)   *pdwPad1   = p1;
    if (pdwPad2)   *pdwPad2   = p2;
    if (pdwSystem) *pdwSystem = 0;
}


/* ---------------------------------------------------------------- *
 *  Other platform stubs                                              *
 * ---------------------------------------------------------------- */

int InfoNES_Menu( void )
{
    /* InfoNES_Main() calls this in a loop. Returning -1 tells the core
       to exit gracefully. NesSystem::ExecuteFrame never enters
       InfoNES_Main, so this is dead code today. */
    return -1;
}

int InfoNES_ReadRom( const char *pszFileName )
{
    /* NesSystem hands ROM data directly to the InfoNES globals,
       bypassing InfoNES_Load entirely. Provided only so the link
       resolves. */
    (void)pszFileName;
    return -1;
}

void InfoNES_ReleaseRom( void )
{
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
