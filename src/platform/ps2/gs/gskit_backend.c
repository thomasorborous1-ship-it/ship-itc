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

#include <kernel.h>
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

/* picodrive-style VBlank handling: a semaphore that the VBlank IRQ
   handler signals on every vsync. Frame code that wants to wait for
   the next display refresh calls GSK_WaitVsync(), which drains any
   stale signal and then WaitSema's on this semaphore. The handler is
   installed via gsKit_add_vsync_handler() so it coexists cleanly with
   any other INTC #3 chain entries (the legacy iaddis hw.s VRstart
   handler stays in place, both increment their own state). */
static int _gsk_vsync_sema_id = -1;
static int _gsk_vsync_cb_id   = -1;

static int _gsk_vsync_handler(int cause)
{
    (void)cause;
    if (_gsk_vsync_sema_id >= 0)
        iSignalSema(_gsk_vsync_sema_id);
    ExitHandler();
    return 0;
}

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

    /* Reset gsKit's VRAM allocator before init_screen so the
       framebuffer / Z buffer / TexManager regions all start at the
       very bottom of VRAM with no holes. picodrive's video_init does
       the same call in the same place; without it gsKit_init_screen
       inherits whatever CurrentPointer was left over from a previous
       initialisation (relevant on hot-reload paths and on real PS2
       hardware where the IOP reset does not zero EE-side state). */
    gsKit_vram_clear(_pGsGlobal);

    gsKit_init_screen(_pGsGlobal);

    /* PMODE / DISPLAY1 / DISPLAY2 are now left at the values that
       gsKit_init_screen programmed (PMODE=0x8046 with CRTMD=1,
       DISPLAY1/2 with gsKit's auto-computed magnification). The
       previous code re-emitted those three registers with the
       iaddis legacy layout (PMODE=0xFF61, DW=2560, MagV=0) to
       work around a NetherSX2-only artefact, but that combination
       puts the PCRTC in a non-standard mode (CRTMD=0, EN1=1,
       EN2=0) that the real PS2 silicon does not handle the same
       way as emulators - on real hardware the picture comes up
       small in the centre with vertical-stripe garbage around it,
       because gsKit_sync_flip only updates DISPFB2 and DISPFB1
       (the only one being read with EN1=1, EN2=0) is left at its
       initial value, breaking double buffering.

       picodrive and Open-PS2-Loader both let gsKit handle PMODE
       and DISPLAY entirely, and both render correctly on real
       PS2. We follow the same pattern.

       If the NetherSX2 visual issue resurfaces, gate the override
       behind a build flag (e.g. -DBUILD_FOR_NETHERSX2=1) instead
       of penalising real hardware. */
    (void)dispx;
    (void)dispy;
    (void)width;
    (void)height;

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

    /* Install the gsKit-side VBlank handler now that the GS is
       producing a video signal. Done before the initial clear so
       the very first GSK_SyncFlip() that follows can wait for VBlank
       through the semaphore path instead of polling CSR.FIELD (which
       on real PS2 takes a couple of refreshes to start updating
       reliably after init_screen). picodrive's video_init does the
       equivalent call in the same place. */
    if (_gsk_vsync_sema_id < 0) {
        ee_sema_t s;
        s.init_count = 0;
        s.max_count  = 1;
        s.option     = 0;
        s.attr       = 0;
        _gsk_vsync_sema_id = CreateSema(&s);
        _gsk_vsync_cb_id   = gsKit_add_vsync_handler(_gsk_vsync_handler);
    }

    /* Single clear is enough — the previous code did this twice with a
       sync_flip in between to "settle" the GS, but the second clear
       was painting the same buffer that gsKit_init_screen had already
       allocated and zero-cleared via the FRAME register write. The
       extra sync_flip on real PS2 hangs because gsKit_sync_flip polls
       CSR.FIELD (bit 13 of GS_CSR at 0x12001000) and that bit takes a
       handful of refreshes to start toggling after init_screen — long
       enough for boot to look frozen with garbage on screen.
       gsKit_finish() waits for the GIF FINISH IRQ (which queue_exec
       appended at the tail of the DMA chain), so the clear is
       guaranteed visible by the time we return, but unlike
       gsKit_vsync_wait / gsKit_sync_flip it does not depend on the
       PCRTC having entered a steady state yet.

       Color is passed as 0 (R=G=B=A=0 in RGBAQ format) instead of
       the GS_BLACK macro: gsKit_clear takes a u64 RGBAQ color and
       the macro is not defined in every gsKit branch we build
       against (it was added later in upstream); 0 produces the same
       black clear and matches what the original SNESticle pre-fork
       code passed here. */
    gsKit_clear(_pGsGlobal, 0);
    gsKit_queue_exec(_pGsGlobal);
    gsKit_finish();

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
    GSGLOBAL *gs = _pGsGlobal;

    if (!_gsk_initialised || !gs) {
        return;
    }

    /* picodrive-style frame swap: the actual DISPFB2 update is done
       here without spinning on CSR.FIELD (which is what the upstream
       gsKit_sync_flip does and which is unreliable on real PS2 — see
       GSK_Init for the long version). The VBlank wait is decoupled
       and runs on a semaphore signalled by _gsk_vsync_handler. */
    if (!gs->FirstFrame && gs->DoubleBuffering == GS_SETTING_ON) {
        GS_SET_DISPFB2(gs->ScreenBuffer[gs->ActiveBuffer & 1] / 8192,
                       gs->Width / 64, gs->PSM, 0, 0);
        gs->ActiveBuffer ^= 1;
    }

    gsKit_setactive(gs);

    /* Wait for the next VBlank IRQ so the user does not see partially
       drawn frames and so we throttle to 60Hz / 50Hz. PollSema drains
       a stale signal first (in case the previous frame already raised
       VBlank between the last WaitSema and this one), then WaitSema
       blocks until the handler fires next. */
    if (_gsk_vsync_sema_id >= 0) {
        PollSema(_gsk_vsync_sema_id);
        WaitSema(_gsk_vsync_sema_id);
    }
}

void GSK_WaitVsync(void)
{
    if (_gsk_vsync_sema_id < 0) {
        return;
    }
    PollSema(_gsk_vsync_sema_id);
    WaitSema(_gsk_vsync_sema_id);
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
