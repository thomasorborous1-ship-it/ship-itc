/* gskit_backend.c
 *
 * gsKit-based replacement for the original direct-GS pipeline.
 * See gskit_backend.h for the public API.
 *
 * Fase 1 GS->gsKit migration.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <gsKit.h>
#include <dmaKit.h>
#include <gsInline.h>
#include <gsToolkit.h>

#include "types.h"
#include "ps2dma.h"
#include "gs.h"
#include "gskit_backend.h"

/* The original headers use these constants for mode / interlace. They
   live in gs.h but we want this TU to compile without dragging the
   register-level header in, so re-declare the values that match. */
#ifndef GS_NTSC
#define GS_NTSC          2
#define GS_PAL           3
#define GS_INTERLACE     1
#define GS_NONINTERLACE  0
#endif

static GSGLOBAL *_pGsGlobal = NULL;
static int       _gsk_initialised = 0;
static int       _gsk_invalidate_pending = 0;

GSGLOBAL *GSK_GetGlobal(void)
{
    return _pGsGlobal;
}

void GSK_Init(int width, int height,
              int dispx, int dispy,
              int psm, int psmz,
              int mode, int interlace)
{
    if (_gsk_initialised) {
        return;
    }

    _pGsGlobal = gsKit_init_global();
    if (!_pGsGlobal) {
        return;
    }

    /* Map iaddis-style mode constants to gsKit's enum.
       The original code uses GS_NTSC=2 / GS_PAL=3 which happen to
       match GS_MODE_NTSC / GS_MODE_PAL exactly, but go through the
       check anyway in case a caller passes something else. */
    _pGsGlobal->Mode = (mode == GS_PAL) ? GS_MODE_PAL : GS_MODE_NTSC;
    _pGsGlobal->Interlace = (interlace == GS_INTERLACE)
                                ? GS_INTERLACED
                                : GS_NONINTERLACED;
    _pGsGlobal->Field = GS_FRAME;

    _pGsGlobal->Width  = width;
    _pGsGlobal->Height = height;
    _pGsGlobal->PSM    = psm;
    _pGsGlobal->PSMZ   = psmz;

    /* Match the original code's behaviour: no Z buffer is needed for
       the 2D blit-style rendering this app does. The Z buffer in the
       old draw_env was only there because GS_SetEnv set it up; the
       actual prims always use TEST_1 with Z disabled. */
    _pGsGlobal->ZBuffering      = GS_SETTING_OFF;
    _pGsGlobal->DoubleBuffering = GS_SETTING_ON;
    _pGsGlobal->PrimAAEnable    = GS_SETTING_OFF;
    _pGsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    _pGsGlobal->Dithering       = GS_SETTING_OFF;
    _pGsGlobal->DrawOrder       = GS_PER_OS;

    /* DMA setup. The SNES blender (snppublend_gs.cpp) also kicks raw
       DMA chains on the GIF channel; gsKit and the blender share the
       same channel, so they must be serialised via GSK_DrainAndWait. */
    /* RCYC=8, no source/dest stall, no MFIFO. The "STS" channel is
       UNSPEC because we do not opt into source-stall behaviour. */
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsKit_init_screen(_pGsGlobal);

    /* gsKit_init_screen has already programmed DISPLAY1/2 with its
       own auto-computed magnification (NTSC default DW=2880, DH=480
       gives MagH=10, MagV=1 for a 256x240 framebuffer). The original
       SNESticle pipeline used a different convention - 1x vertical
       and ~10x horizontal magnification, with DW=2559 and DH=h-1 -
       which yields a noticeably different visible aspect on real TV
       and on emulators that decode DISPLAY1 strictly (NetherSX2
       reports the picture as oversized).

       Re-emit DISPLAY1/2 with the legacy register layout so the
       picture comes out at the same scale the iaddis original used.
       gsKit does not touch DISPLAY1/2 again after init_screen, so
       this stays in effect. */
    {
        int w = width  ? width  : 256;
        int h = height ? height : 240;
        u64 disp_reg = (((u64)((u64)(h - 1)) << 44) |
                        ((u64)0x9FFULL << 32) |
                        ((u64)(((2560 + w - 1) / w) - 1) << 23) |
                        ((u64)(dispy & 0x7FF) << 12) |
                        ((u64)(dispx * (2560 / w)) & 0xFFFULL));
        *((volatile u64 *)0x12000080) = disp_reg; /* DISPLAY1 */
        *((volatile u64 *)0x120000A0) = disp_reg; /* DISPLAY2 */
        /* Keep gsGlobal's idea of the centre roughly aligned in case
           a future caller of gsKit_set_display_offset uses it. */
        _pGsGlobal->StartX = dispx * (2560 / w);
        _pGsGlobal->StartY = dispy;
        _pGsGlobal->MagH   = ((2560 + w - 1) / w) - 1;
        _pGsGlobal->MagV   = 0;
        _pGsGlobal->DW     = 2560;
        _pGsGlobal->DH     = h;
    }

    /* Re-emit PMODE with the iaddis layout (0xFF61).
       gsKit_init_screen unconditionally programs PMODE with
       EN1=0, EN2=1, CRTMD=1, MMOD=0, AMOD=1, SLBG=0, ALP=0x80
       which yields 0x8046. On NetherSX2 (Patched) and other strict
       emulators, CRTMD=1 combined with our 256x240 framebuffer
       layout produces the same vertical-stripe / partial-frame
       readout artefact that PR #41 fixed pre-gsKit-migration by
       forcing PMODE=0xFF61 in GS_SetDispMode. The gsKit migration
       removed the explicit GS_PMODE = 0xFF61 write in gs.c, which
       silently reintroduced that regression.

       0xFF61 layout: EN1=1, EN2=0, CRTMD=0, MMOD=1, AMOD=1, SLBG=0,
       ALP=0xFF - i.e. Read Circuit 1 reads DISPFB1 with ALP=255
       (full opacity on RC1's output), CRTMD=0 (real-PS2-silicon
       friendly), no SLBG. gsKit sets DISPFB1 and DISPFB2 to the
       same gsKit-managed framebuffer base, so flipping EN1<->EN2
       does not change which texels reach the CRTC. */
    *((volatile u64 *)0x12000000) = 0xFF61ULL; /* PMODE */

    /* COLCLAMP is re-emitted every frame in GSK_ResetFrame (see
       comment there). The original iaddis pipeline (gs.c) set
       COLCLAMP=1 as part of GS_SetEnv but gsKit_init_screen does
       not touch it, so it sits at the GS reset default (0). */

    gsKit_set_test (_pGsGlobal, GS_ZTEST_OFF);
    gsKit_set_clamp(_pGsGlobal, GS_CMODE_REPEAT);
    gsKit_set_primalpha(_pGsGlobal,
        GS_SETREG_ALPHA(0, 1, 0, 1, 0x80), 0); /* Cs*As + Cd*(1-As) */

    gsKit_TexManager_init(_pGsGlobal);
    gsKit_mode_switch(_pGsGlobal, GS_ONESHOT);

    /* Clear both buffers so the screen starts black. */
    gsKit_clear(_pGsGlobal, 0);
    gsKit_queue_exec(_pGsGlobal);
    gsKit_finish();
    gsKit_sync_flip(_pGsGlobal);
    gsKit_clear(_pGsGlobal, 0);
    gsKit_queue_exec(_pGsGlobal);
    gsKit_finish();
    gsKit_sync_flip(_pGsGlobal);

    _gsk_initialised = 1;
}

Uint32 GSK_VramAllocTBP(Uint32 nBytes)
{
    u32 addr;

    if (!_gsk_initialised || !_pGsGlobal) {
        return 0;
    }

    addr = gsKit_vram_alloc(_pGsGlobal, nBytes, GSKIT_ALLOC_USERBUFFER);
    if (addr == GSKIT_ALLOC_ERROR) {
        return 0;
    }
    return addr / 256;
}

void GSK_DrainAndWait(void)
{
    if (!_gsk_initialised) {
        return;
    }

    gsKit_queue_exec(_pGsGlobal);
    gsKit_finish();
    DmaSyncGIF();
}

void GSK_FlushFrame(void)
{
    if (!_gsk_initialised) {
        return;
    }

    gsKit_queue_exec(_pGsGlobal);
    gsKit_finish();
}

void GSK_SyncFlip(void)
{
    if (!_gsk_initialised) {
        return;
    }

    gsKit_sync_flip(_pGsGlobal);
}

void GSK_ResetFrame(void)
{
    GSGLOBAL *gs;
    u64 *p_data;

    if (!_gsk_initialised || !_pGsGlobal) {
        return;
    }

    gs = _pGsGlobal;

    /* Allocate a four-register A+D GIF tag in gsKit's heap.  The
       queue will dispatch it before any subsequent prim, so FRAME_1,
       XYOFFSET_1, ALPHA_1 and COLCLAMP are all refreshed before
       drawing actually happens.

       XYOFFSET_1 must be restored here because the SNES per-scanline
       blender (snppublend_gs.cpp) overwrites it on every Exec() call
       with a line-specific value (0x8000, 0x8000 - iLine*16).  The
       blender's End() restores it through the GPFifo chain, but that
       chain is dispatched *after* gsKit's queue has already drained
       (see GPFifoPause → GSK_DrainAndWait ordering).  Any gsKit
       textured prim queued between End() and GPFifoFlush therefore
       draws with the blender's stale XYOFFSET, which shifts the
       sprite hundreds of pixels off-screen — the visible symptom is a
       permanently frozen menu image because the game output never
       lands inside the visible framebuffer area.

       ALPHA_1 must be restored here for the symmetric reason:  the
       blender's per-scanline DMA chain rewrites ALPHA_1 several times
       (snppublend_gs.cpp _SNPPUBlendBuildList) and leaves it at
       GS_SET_ALPHA(1, 2, 0, 2, 0x80), i.e. output = (Cd - 0) * As + 0
       = Cd * As.  The blender's End() does *not* restore ALPHA_1, and
       gsKit's prim helpers (gsKit_prim_sprite,
       gsKit_prim_sprite_texture_3d, ...) emit only PRIM / color / XY
       per draw — they never re-emit ALPHA_1.  The gsKit init value
       set via gsKit_set_primalpha (GS_SETREG_ALPHA(0, 1, 0, 1, 0x80)
       = standard (Cs - Cd) * As + Cd) therefore stays clobbered for
       the rest of the session.  Any subsequent gsKit prim drawn with
       ABE = 1 (every font draw, every PolyBlend(TRUE) rect, the menu
       selection bar, the "SRAM saved." modal, ...) ends up computing
       output = Cd, which leaves the framebuffer unchanged and makes
       the entire menu overlay invisible — the visible symptom on the
       L2+R2 game-exit path is a frozen darkened game frame with no
       menu UI on top, while audio and input keep responding.  This
       is the same class of bug PR #60 fixed for FRAME_1 / XYOFFSET_1
       but in the opposite (game → menu) direction. */
    /* COLCLAMP = 1 clamps per-channel alpha-blend / colour-math
       results to 0..255.  The GS reset default is 0 (wrap on
       overflow); the original iaddis pipeline (gs.c) programmed
       COLCLAMP = 1 in GS_SetEnv but the gsKit migration dropped
       that write.  Without it, any final composition draw that
       saturates a channel (sprite or BG2/BG3 region overlapping
       BG1 with alpha) ends up wrapping the high bits, which on
       the visible framebuffer appears as banded/striped corruption
       in those regions while BG1-only pixels (the borders) stay
       intact.  Restoring it per-frame here matches the cadence of
       the FRAME / XYOFFSET / ALPHA restores. */
    p_data = (u64 *)gsKit_heap_alloc(gs, 4, 64, GIF_AD);
    if (!p_data) {
        return;
    }

    *p_data++ = GIF_TAG_AD(4);
    *p_data++ = GIF_AD;
    *p_data++ = GS_SETREG_FRAME_1(
        gs->ScreenBuffer[gs->ActiveBuffer & 1] / 8192,
        gs->Width / 64,
        gs->PSM,
        0);
    *p_data++ = GS_REG_FRAME_1;
    *p_data++ = GS_SETREG_XYOFFSET_1(gs->OffsetX, gs->OffsetY);
    *p_data++ = GS_XYOFFSET_1;
    /* Standard alpha blend: output = (Cs - Cd) * As + Cd.  Matches the
       value gsKit_set_primalpha() programmed at GSK_Init() time. */
    *p_data++ = GS_SETREG_ALPHA(0, 1, 0, 1, 0x80);
    *p_data++ = GS_REG_ALPHA_1;
    /* COLCLAMP = 1 (clamp).  Register 0x46 takes a single bit. */
    *p_data++ = (u64)1;
    *p_data++ = (u64)GS_REG_COLCLAMP;
}

void GSK_InvalidateTextureCache(void)
{
    _gsk_invalidate_pending = 1;
}

/* Internal: callers in gpprim.c should consult this and emit a
   TEXFLUSH register write before binding a texture if a blender
   chain has run since the last bind. */
int GSK_TakeInvalidatePending(void)
{
    int p = _gsk_invalidate_pending;
    _gsk_invalidate_pending = 0;
    return p;
}

void *GSK_AsUncached(void *ptr)
{
    Uint32 addr = (Uint32)ptr;

    /* NULL stays NULL. */
    if (!addr) {
        return ptr;
    }

    /* Only KSEG0 / KUSEG cached pointers (top nibble 0x0..0x1, i.e.
       byte address < 0x20000000) can be aliased through KSEG1 by
       setting bit 29. Anything in 0x20000000+ is already uncached
       (KSEG1) or is a kernel/io segment that must not be touched
       through the alias trick.

       PS2 main RAM is 32MB at 0x00000000-0x01FFFFFF, so any legitimate
       pointer into a buffer the EE allocates falls well below the
       0x10000000 threshold. We assert a tighter bound (<256MB) to
       catch accidental use with stack/scratchpad/IO addresses while
       still allowing future memory-map changes. The assert is
       compile-out in CODE_RELEASE so it has zero hot-path cost. */
    assert((addr & 0xF0000000) == 0 &&
           "GSK_AsUncached: pointer is outside physical RAM (<256MB)");

    return (void *)(addr | 0x20000000);
}
