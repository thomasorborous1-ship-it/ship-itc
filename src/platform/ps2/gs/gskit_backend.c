/* gskit_backend.c
 *
 * gsKit-based replacement for the original direct-GS pipeline.
 * See gskit_backend.h for the public API.
 *
 * Fase 1 GS->gsKit migration.
 */

#include <stdio.h>
#include <string.h>

#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>

#include "types.h"
#include "ps2dma.h"
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

    /* Position the framebuffer on the TV screen. The original code
       used dx*(2560/width) for the X offset, baked into DISPLAY1.
       gsKit's StartX/StartY get a sensible per-mode default; we then
       nudge by (requested - default) so the picture lands where the
       app expects. */
    {
        int extra_x = (dispx * (2560 / (width ? width : 256)))
                      - _pGsGlobal->StartX;
        int extra_y = dispy - _pGsGlobal->StartY;
        gsKit_set_display_offset(_pGsGlobal, extra_x, extra_y);
    }

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
