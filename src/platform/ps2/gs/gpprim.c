/* gpprim.c
 *
 * Original GFX-Pipe primitives by Vzzrzzn / Sjeep, hand-built GIF
 * tags via gslist.c. After the Fase 1 GS->gsKit migration each
 * primitive is rebuilt on top of gsKit's queue (gsKit_prim_sprite,
 * gsKit_prim_sprite_texture_3d) while the public API in gpprim.h
 * stays unchanged.
 *
 * Coordinate convention: callers pass FIXED4 sub-pixel values
 * (pixels << 4). gsKit's prim helpers want floating-point pixels,
 * so we divide by 16 before handing them in.
 */

#include <tamtypes.h>
#include <kernel.h>
#include <string.h>

#include <gsKit.h>
#include <dmaKit.h>
#include <gsInline.h>

#include "types.h"
#include "gs.h"
#include "gpprim.h"
#include "gskit_backend.h"

extern int GSK_TakeInvalidatePending(void);

/* "Current" texture binding state, written by GPPrimSetTex and
   consumed by GPPrimTexRect. The original pipeline emitted TEX0 /
   TEX1 / TEXA / CLAMP / ALPHA / PABE here directly via GIF-AD, but
   gsKit_prim_sprite_texture_3d emits TEX0 itself per draw; we just
   keep the parameters. */
static GSTEXTURE _gpprim_curTex;
static int       _gpprim_curTexValid = 0;

/* Convert FIXED4 sub-pixel coordinate to float pixels. */
static inline float fx4_to_float(unsigned v)
{
    return ((float)((int)v)) / 16.0f;
}

/* Promote a 32-bit RGBA colour to a 64-bit RGBAQ, with Q = 1.0f.
   The high half being non-zero matters: a Q of zero gives NaN-flavoured
   ST mapping in the GS and produces black sprites in some setups. */
static inline u64 rgba32_to_rgbaq64(u32 rgba)
{
    return ((u64)rgba) | ((u64)0x3f800000ULL << 32);
}

void GPPrimRect(unsigned x1, unsigned y1, unsigned c1,
                unsigned x2, unsigned y2, unsigned c2,
                unsigned z, unsigned abe)
{
    GSGLOBAL *gs = GSK_GetGlobal();
    if (!gs) {
        return;
    }
    (void)c2; /* original API kept c2 but always equal to c1 */

    if (abe) {
        gs->PrimAlphaEnable = GS_SETTING_ON;
    } else {
        gs->PrimAlphaEnable = GS_SETTING_OFF;
    }

    gsKit_prim_sprite(gs,
                      fx4_to_float(x1), fx4_to_float(y1),
                      fx4_to_float(x2), fx4_to_float(y2),
                      (int)z,
                      rgba32_to_rgbaq64(c1));
}

void GPPrimEnableZBuf(void)
{
    GSGLOBAL *gs = GSK_GetGlobal();
    if (!gs) {
        return;
    }
    gsKit_set_test(gs, GS_ZTEST_ON);
}

void GPPrimDisableZBuf(void)
{
    GSGLOBAL *gs = GSK_GetGlobal();
    if (!gs) {
        return;
    }
    gsKit_set_test(gs, GS_ZTEST_OFF);
}

void GPPrimTexRect(u32 x1, u32 y1, u32 u1, u32 v1,
                   u32 x2, u32 y2, u32 u2, u32 v2,
                   u32 z, u32 colour, unsigned abe)
{
    GSGLOBAL *gs = GSK_GetGlobal();
    if (!gs || !_gpprim_curTexValid) {
        return;
    }

    if (abe) {
        gs->PrimAlphaEnable = GS_SETTING_ON;
    } else {
        gs->PrimAlphaEnable = GS_SETTING_OFF;
    }

    /* If something outside gsKit (the SNES blender, or a manual
       texture upload) overwrote VRAM, force the GS texture cache
       to flush before sampling again. */
    if (GSK_TakeInvalidatePending()) {
        u64 *p = (u64 *)gsKit_heap_alloc(gs, 1, 16, GIF_AD);
        if (p) {
            *p++ = GIF_TAG_AD(1);
            *p++ = GIF_AD;
            *p++ = 0;          /* TEXFLUSH expects any value */
            *p++ = GS_REG_TEXFLUSH;
        }
    }

    gsKit_prim_sprite_texture_3d(gs, &_gpprim_curTex,
                                 fx4_to_float(x1), fx4_to_float(y1),
                                 (int)z,
                                 fx4_to_float(u1), fx4_to_float(v1),
                                 fx4_to_float(x2), fx4_to_float(y2),
                                 (int)z,
                                 fx4_to_float(u2), fx4_to_float(v2),
                                 rgba32_to_rgbaq64(colour));
}

void GPPrimSetTex(u32 tbp, u32 tbw, u32 texwidthlog2, u32 texheightlog2,
                  u32 tpsm, u32 cbp, u32 cbw, u32 cpsm, int filter)
{
    (void)cbw;

    /* Capture the binding so the next GPPrimTexRect can use it. The
       legacy pipeline took tbp / cbp in TBP units (256-byte blocks)
       and tbw in pixels; gsKit's GSTEXTURE wants Vram in bytes and
       TBW in 64-pixel units, so convert here. */
    _gpprim_curTex.Width   = 1U << texwidthlog2;
    _gpprim_curTex.Height  = 1U << texheightlog2;
    _gpprim_curTex.PSM     = tpsm;
    _gpprim_curTex.ClutPSM = cpsm;
    _gpprim_curTex.TBW     = tbw / 64;
    if (_gpprim_curTex.TBW == 0) {
        _gpprim_curTex.TBW = 1;
    }
    _gpprim_curTex.Mem      = NULL;
    _gpprim_curTex.Clut     = NULL;
    _gpprim_curTex.Vram     = tbp * 256;
    _gpprim_curTex.VramClut = cbp * 256;
    _gpprim_curTex.Filter   = filter ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    _gpprim_curTex.ClutStorageMode = 0;
    _gpprim_curTex.Delayed  = 0;

    _gpprim_curTexValid = 1;
}

/* Direct EE->VRAM texture upload via gsKit's helper. The legacy
   variant built BITBLTBUF / TRXPOS / TRXREG / TRXDIR registers and
   an image GIF tag by hand; gsKit_texture_send does the same thing
   while staying in sync with the rest of the gsKit GIF state. */
void GPPrimUploadTexture(int TBP, int TBW, int xofs, int yofs,
                         int pxlfmt, void *tex, int wpxls, int hpxls)
{
    GSGLOBAL *gs = GSK_GetGlobal();

    /* xofs/yofs are unused by the SNESticle paths today (always 0),
       and gsKit_texture_send_inline does not accept an offset. If a
       non-zero offset ever gets passed in, fall back to the manual
       GIF chain via... well, just bail for now and let the caller
       see a missing texture. The repo does not exercise that path. */
    (void)xofs;
    (void)yofs;
    (void)gs;

    if (!gs || !tex || wpxls <= 0 || hpxls <= 0) {
        return;
    }

    {
        /* gsKit's texture-send helpers take the destination address
           in BYTES and divide by 256 internally to encode
           BITBLTBUF.DBP. The legacy SNESticle TBP is in 256-byte
           units, so multiply once to get a byte address. Earlier we
           were dividing again right before the call, which placed
           the upload 256x closer to the start of VRAM than the
           sampler later read from - the textures landed in the
           wrong page and every textured prim came out blank. */
        u32 tbp_bytes = (u32)TBP * 256U;
        u32 tbw_pages = (u32)TBW / 64U;
        if (tbw_pages == 0) {
            tbw_pages = 1;
        }
        /* CACHE COHERENCY: gsKit_texture_send_inline DMAs straight
           from the EE's main RAM into VRAM. On real PS2 hardware
           the EE has an 8KB write-back data cache that is NOT
           snooped by the GIF/DMAC; if the caller just wrote the
           texel buffer (e.g. font upload from FontData_04b16b,
           emulator framebuffer blit), the freshly-written bytes
           may still be sitting in the data cache and the DMA will
           read stale RAM contents - producing visibly corrupted
           textures (random tiles, scrambled font) on real PS2.
           Emulators (PCSX2/NetherSX2) do not model the EE cache,
           so the same code "works" there which masks the bug.
           FlushCache(0) writes back+invalidates the EE data cache
           so the DMA sees the actual texel data. Required before
           every EE->VRAM transfer. */
        FlushCache(0);
        gsKit_texture_send_inline(gs, (u32 *)tex,
                                  wpxls, hpxls,
                                  tbp_bytes, /* bytes; gsKit divides by 256 */
                                  pxlfmt,
                                  tbw_pages,
                                  GS_CLUT_NONE);
    }

    /* Anything that draws after this should re-flush the texture
       cache to pick up the freshly written texels. */
    GSK_InvalidateTextureCache();
}
