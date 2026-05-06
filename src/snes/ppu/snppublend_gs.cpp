/* snppublend_gs.cpp
 *
 * Per-line SNES PPU blender, GS->gsKit Fase 2 migration.
 *
 * The legacy implementation built a static GIF chain in EE memory once
 * per blend-info change and patched four pointers per scanline before
 * kicking the chain on the GIF DMA channel directly. After Fase 1
 * (gsKit owns the framebuffer) keeping that path alive required
 * draining gsKit's queue around every Begin/End pair and emitting a
 * manual TEXFLUSH afterwards.
 *
 * In Fase 2 the chain is allocated per-Exec from gsKit's drawbuffer
 * heap via gsKit_heap_alloc (for AD register blocks) and
 * gsKit_heap_alloc_dma (for image-mode DMA REF tags). Submission is
 * done through gsKit_queue_exec + gsKit_finish, sharing the GIF
 * channel ownership with the rest of the renderer. The static
 * SNPPUDmaListT.Data buffer and the four "patch pointers" disappear.
 *
 * VRAM layout (TBP units, 256 byte blocks) is unchanged from the
 * iaddis original so the SnesPPURender constructor still passes
 * (0x3C00, 0x2400):
 *   uPalAddr        = vram + 0x000
 *   uInputAddr      = vram + 0x080  (main8 line 0, sub8 line 1, attrib8 line 2)
 *   uAttribMainPal  = vram + 0x180
 *   uAttribSubPal   = vram + 0x184
 *   uTempAddr       = vram + 0x200
 *   uOutAddr        = caller-supplied (output texture)
 */

#include <stdlib.h>
#include "types.h"
#include "prof.h"
#include "snmask.h"
#include "rendersurface.h"
#include "snppurender.h"
#include "snppublend_gs.h"
#include "snppucolor.h"

#include <tamtypes.h>
extern "C" {

#include <kernel.h>
#include <gsKit.h>
#include <gsInline.h>
#include <dmaKit.h>
#include "gskit_backend.h"
#include "gs.h"
#include "ps2mem.h"

}

#define SNPPUBLEND_PAL32 (TRUE)

extern SnesChrLookupT _SnesPPU_PlaneLookup[2];

/* Constant attribute-mask palettes. Only the first 8 entries (3-bit
 * attribute index into HSM) are ever sampled, but we upload them as
 * 16x16 PSMCT32 to match the legacy CSM1 layout, exactly like the
 * iaddis original did. */
static Uint32 _SNPPUBlend_AttribMainPal[8] _ALIGN(16) =
{                   /* HSM */
    0x00000000,     /* 000 */
    0x80000000,     /* 001 */
    0x00000000,     /* 010 */
    0x80000000,     /* 011 */
    0x00000000,     /* 100 */
    0x40000000,     /* 101 */
    0x00000000,     /* 110 */
    0x40000000,     /* 111 */
};


static Uint32 _SNPPUBlend_AttribSubPal[8] _ALIGN(16) =
{                   /* HSM */
    0x00000000,     /* 000 */
    0x00000000,     /* 001 */
    0x80000000,     /* 010 */
    0x80000000,     /* 011 */
    0x00000000,     /* 100 */
    0x00000000,     /* 101 */
    0x40000000,     /* 110 */
    0x40000000,     /* 111 */
};



static void _PlanarTo3(Uint8 *pDest, SNMaskT *pSrc0, SNMaskT *pSrc1, SNMaskT *pSrc2)
{
	Uint32 nBytes = 256 / 8;
	SnesChrLookup64T *pLookup64 = (SnesChrLookup64T *)&_SnesPPU_PlaneLookup[1];
	Uint64 *pDest64 = (Uint64 *)pDest;


	while (nBytes > 0)
	{
		Uint64 uData;

		uData  = (*pLookup64)[pSrc0->uMask8[0]] << 0;	
		uData |= (*pLookup64)[pSrc1->uMask8[0]] << 1;	
		uData |= (*pLookup64)[pSrc2->uMask8[0]] << 2;	


		pSrc0  = (SNMaskT *) (((Uint8 *)pSrc0) + 1);
		pSrc1  = (SNMaskT *) (((Uint8 *)pSrc1) + 1);
		pSrc2  = (SNMaskT *) (((Uint8 *)pSrc2) + 1);

		pDest64[0] = uData;
		pDest64+=1;

		nBytes--;
	}

}

#if SNPPUBLEND_PAL32

void SNPPUBlendGS::UpdatePaletteEntry(SNPPUBlendInfoT *pInfo, Uint32 uAddr, Uint32 uData, Uint32 uIntensity)
{
    PaletteT *pPal = pInfo->Pal;

	uData = SNPPUColorConvert15to32(uData & 0x7FFF);

	if (uAddr > 0)
	{
		uData |= 0x80000000;
	} 

	/* swap 8 and 0x10 of addr */
	uAddr = (uAddr & ~0x18) | ((uAddr & 0x10) >> 1) | ((uAddr & 0x08) << 1);

	pPal->Color32[uAddr] = uData;
}

void SNPPUBlendGS::UpdatePalette(SNPPUBlendInfoT *pInfo, Uint16 *pCGRam, Uint32 uIntensity)
{
	Int32 iEntry;
    PaletteT *pPal = pInfo->Pal;

	PROF_ENTER("SNPPUBlendUpdatePalette");

	pPal->Color32[0] = SNPPUColorConvert15to32(pCGRam[0]);
	for (iEntry=1; iEntry < 256; iEntry++)
	{
		Uint32 uAddr = iEntry;

		uAddr = (uAddr & ~0x18) | ((uAddr & 0x10) >> 1) | ((uAddr & 0x08) << 1);

		/* set palette entry (with alpha set) */
		pPal->Color32[uAddr] = SNPPUColorConvert15to32(pCGRam[iEntry]) | 0x80000000;
	}

	PROF_LEAVE("SNPPUBlendUpdatePalette");
}

#else

static Uint32 SNPPUColorConvert15to32(SnesColor16T uColor16)
{
	Uint32 uColor32;
	Uint32 uR, uG, uB;

	uR = ((uColor16 >>  0) & 0x1F);
	uG = ((uColor16 >>  5) & 0x1F);
	uB = ((uColor16 >>  10) & 0x1F);

	/* convert snes16->generic32 */
	uColor32 =  uR <<  (0  + 3);
	uColor32|=  uG <<  (8  + 3);
	uColor32|=  uB <<  (16 + 3);
	return uColor32;
}


void SNPPUBlendGS::UpdatePaletteEntry(SNPPUBlendInfoT *pInfo, Uint32 uAddr, Uint32 uData, Uint32 uIntensity)
{
    PaletteT *pPal = pInfo->Pal;
	if (uAddr > 0)
	{
		uData |= 0x8000;
	} 
	pPal->Color16[uAddr] = uData;
}

void SNPPUBlendGS::UpdatePalette(SNPPUBlendInfoT *pInfo, Uint16 *pCGRam, Uint32 uIntensity)
{
	Int32 iEntry;
    PaletteT *pPal = pInfo->Pal;

	PROF_ENTER("SNPPUBlendUpdatePalette");


	pPal->Color16[0] = pCGRam[0];
	for (iEntry=1; iEntry < 256; iEntry++)
	{
		/* set palette entry (with alpha set) */
		pPal->Color16[iEntry] = pCGRam[iEntry] | 0x8000;
	}

	PROF_LEAVE("SNPPUBlendUpdatePalette");
}



#endif


/* ----------------------------------------------------------------- */
/* gsKit chain helpers                                                */
/* ----------------------------------------------------------------- */

/* Allocate one A+D GIF block in gsKit's drawbuffer heap. Returns a
 * pointer past the GIFTAG that the caller should fill with `nregs`
 * (data, register) pairs. Using GIF_AD as the type guarantees a fresh
 * GIFTAG every call - we don't want gsKit's auto-merge here, the
 * legacy chain wrote each register block as its own tag and the GS
 * pipeline depends on that ordering. */
static u64 *_alloc_ad(GSGLOBAL *gs, int nregs)
{
    u64 *p = (u64 *)gsKit_heap_alloc(gs, nregs, nregs * 16, GIF_AD);
    *p++ = GIF_TAG_AD(nregs);
    *p++ = GIF_AD;
    return p;
}

/* Number of qwords occupied by an image of `w` x `h` pixels in `psm`. */
static int _image_qwords(int psm, int w, int h)
{
    int n = w * h;
    switch (psm)
    {
    case GS_PSMCT32: return (n + 3) >> 2;   /* 4 px / qword  */
    case GS_PSMCT16: return (n + 7) >> 3;   /* 8 px / qword  */
    case GS_PSMT8:   return (n + 15) >> 4;  /* 16 px / qword */
    case GS_PSMT4:   return (n + 31) >> 5;  /* 32 px / qword */
    }
    return 0;
}

/* Emit a HOST->LOCAL texture upload via the gsKit heap. Mirrors the
 * legacy _GPFifoUploadTexture: BITBLTBUF / TRXPOS / TRXREG / TRXDIR
 * setup as an A+D block, then an image GIFTAG followed by a DMA REF
 * pointing at the source data in EE memory.
 *
 *   tbp_units   : destination VRAM TBP, in 256-byte units
 *   tbw_pixels  : destination buffer width in pixels (must be multiple of 64)
 *   xofs/yofs   : destination offset in pixels
 *   psm         : destination pixel format (GS_PSMCT32 / PSMCT16 / PSMT8)
 *   src_uncached: pointer into EE memory, already OR'd with 0x80000000
 *   width/height: transfer size in pixels
 */
static void _emit_upload(GSGLOBAL *gs,
                         Uint32 tbp_units, Uint32 tbw_pixels,
                         int xofs, int yofs,
                         int psm,
                         void *src_uncached,
                         int width, int height)
{
    u64 *p = _alloc_ad(gs, 4);
    *p++ = GS_SETREG_BITBLTBUF(0, 0, 0, tbp_units, tbw_pixels / 64, psm);
    *p++ = GS_BITBLTBUF;
    *p++ = GS_SETREG_TRXPOS(0, 0, xofs, yofs, 0);
    *p++ = GS_TRXPOS;
    *p++ = GS_SETREG_TRXREG(width, height);
    *p++ = GS_TRXREG;
    *p++ = GS_SETREG_TRXDIR(0);
    *p++ = GS_TRXDIR;

    int qwc     = _image_qwords(psm, width, height);
    int packets = qwc / GS_GIF_BLOCKSIZE;
    int remain  = qwc % GS_GIF_BLOCKSIZE;
    int dmasize = (packets * 3) + (remain ? 3 : 0);
    if (dmasize == 0)
    {
        return;
    }

    u64 *dp = (u64 *)gsKit_heap_alloc_dma(gs, dmasize, dmasize * 16);
    u32 src_addr = (u32)src_uncached;

    int i;
    for (i = 0; i < packets; i++)
    {
        *dp++ = DMA_TAG(1, 0, DMA_CNT, 0, 0, 0);
        *dp++ = 0;
        *dp++ = GIF_TAG(GS_GIF_BLOCKSIZE, 0, 0, 0, GSKIT_GIF_FLG_IMAGE, 0);
        *dp++ = 0;
        *dp++ = DMA_TAG(GS_GIF_BLOCKSIZE, 0, DMA_REF, 0, src_addr, 0);
        *dp++ = 0;
        src_addr += GS_GIF_BLOCKSIZE * 16;
    }
    if (remain > 0)
    {
        *dp++ = DMA_TAG(1, 0, DMA_CNT, 0, 0, 0);
        *dp++ = 0;
        *dp++ = GIF_TAG(remain, 0, 0, 0, GSKIT_GIF_FLG_IMAGE, 0);
        *dp++ = 0;
        *dp++ = DMA_TAG(remain, 0, DMA_REF, 0, src_addr, 0);
        *dp++ = 0;
    }
}

/* Emit a 256-pixel-wide textured sprite covering one scanline. The
 * caller is responsible for setting TEX0 / ALPHA before this call.
 * `iDestLine` is the destination Y, `iSrcLine` is the source V. The
 * 0x8000 offsets recreate the legacy "pixel-perfect" XYOFFSET
 * convention so that the same XYOFFSET base value used for every
 * line works correctly. */
static void _emit_tex_line(GSGLOBAL *gs, int iDestLine, int iSrcLine,
                           Uint32 RGBA, int abe)
{
    int x1 = (  0 << 4) + 0x8000;
    int y1 = (iDestLine       << 4) + 0x8000;
    int x2 = (256 << 4) + 0x8000;
    int y2 = ((iDestLine + 1) << 4) + 0x8000;

    int u1 =   0           << 4;
    int v1 = (iSrcLine     ) << 4;
    int u2 = 256           << 4;
    int v2 = (iSrcLine + 1) << 4;

    u64 *p = _alloc_ad(gs, 6);
    *p++ = GS_SETREG_PRIM(0x06, 0, 1, 0, abe, 0, 1, 0, 0);  *p++ = GS_PRIM;
    *p++ = (u64)RGBA;                                       *p++ = GS_RGBAQ;
    *p++ = GS_SETREG_UV(u1, v1);                            *p++ = GS_UV;
    *p++ = GS_SETREG_XYZ(x1, y1, 0);                        *p++ = GS_XYZ2;
    *p++ = GS_SETREG_UV(u2, v2);                            *p++ = GS_UV;
    *p++ = GS_SETREG_XYZ(x2, y2, 0);                        *p++ = GS_XYZ2;
}

/* Emit a 256-pixel-wide untextured sprite covering one scanline. The
 * caller is responsible for setting RGBAQ / ALPHA before calling. */
static void _emit_solid_line(GSGLOBAL *gs, int iDestLine, int abe)
{
    int x1 = (  0 << 4) + 0x8000;
    int y1 = (iDestLine       << 4) + 0x8000;
    int x2 = (256 << 4) + 0x8000;
    int y2 = ((iDestLine + 1) << 4) + 0x8000;

    u64 *p = _alloc_ad(gs, 3);
    *p++ = GS_SETREG_PRIM(0x06, 0, 0, 0, abe, 0, 1, 0, 0);  *p++ = GS_PRIM;
    *p++ = GS_SETREG_XYZ(x1, y1, 0);                        *p++ = GS_XYZ2;
    *p++ = GS_SETREG_XYZ(x2, y2, 0);                        *p++ = GS_XYZ2;
}


/* ----------------------------------------------------------------- */
/* Lifecycle: ctor / Begin / End                                      */
/* ----------------------------------------------------------------- */

SNPPUBlendGS::SNPPUBlendGS(Uint32 uVramAddr, Uint32 uOutAddr)
{
    m_uPalAddr        = uVramAddr + 0x000;
    m_uInputAddr      = uVramAddr + 0x080;
    m_uAttribMainPal  = uVramAddr + 0x180;
    m_uAttribSubPal   = uVramAddr + 0x184;
    m_uTempAddr       = uVramAddr + 0x200;
    m_uOutAddr        = uOutAddr;
    m_pTarget         = NULL;
}

void SNPPUBlendGS::Begin(CRenderSurface *pTarget)
{
    m_pTarget = pTarget;
    if (!m_pTarget)
    {
        return;
    }

    GSGLOBAL *gs = GSK_GetGlobal();
    if (!gs)
    {
        return;
    }

    /* Drain anything the UI / mainloop may have queued before us so we
     * have a clean drawbuffer heap to build the per-line chain from. */
    GSK_DrainAndWait();

    /* Upload the constant attribute-mask palettes once per render
     * surface. The legacy code uploads them as 16x16 PSMCT32 even
     * though only 8 entries are actually sampled by attrib8 - keep
     * that exact layout so the CSM1 cache reads land at the same
     * VRAM offsets. */
    _emit_upload(gs, m_uAttribMainPal, 64, 0, 0, GS_PSMCT32,
                 (void *)((Uint32)_SNPPUBlend_AttribMainPal | 0x80000000),
                 16, 16);

    _emit_upload(gs, m_uAttribSubPal, 64, 0, 0, GS_PSMCT32,
                 (void *)((Uint32)_SNPPUBlend_AttribSubPal | 0x80000000),
                 16, 16);

    /* Per-surface render state: TEXCLUT for CSM1, TEXA opaque, no
     * clamp, default TEX1. These only need to be set once before the
     * blender loop runs. */
    {
        u64 *p = _alloc_ad(gs, 4);
        *p++ = (u64)(256 / 64);                          *p++ = GS_TEXCLUT;
        *p++ = GS_SETREG_TEXA(0x00, 0, 0x80);            *p++ = GS_TEXA;
        *p++ = GS_SETREG_CLAMP(0, 0, 0, 0, 0, 0);        *p++ = GS_CLAMP_1;
        *p++ = (u64)0x000;                               *p++ = GS_TEX1_1;
    }

    gsKit_queue_exec(gs);
    gsKit_finish();
}

void SNPPUBlendGS::End()
{
    if (!m_pTarget)
    {
        return;
    }

    GSGLOBAL *gs = GSK_GetGlobal();
    if (!gs)
    {
        m_pTarget = NULL;
        return;
    }

    /* Restore FRAME_1 / XYOFFSET_1 to whatever gsKit currently has
     * configured for its UI rendering. The blender shifted both
     * during the per-line passes. */
    {
        u64 *p = _alloc_ad(gs, 2);
        *p++ = GS_GetFrameReg();    *p++ = GS_FRAME_1;
        *p++ = GS_GetOffsetReg();   *p++ = GS_XYOFFSET_1;
    }

    gsKit_queue_exec(gs);
    gsKit_finish();

    m_pTarget = NULL;
}


/* ----------------------------------------------------------------- */
/* Per-line chain build & submit                                      */
/* ----------------------------------------------------------------- */

void SNPPUBlendGS::Exec(SNPPUBlendInfoT *pInfo, Int32 iLine, Uint32 uFixedColor16,
                       SNMaskT *pColorMask, Bool bAddSub, Uint32 uIntensity)
{
    if (!m_pTarget)
    {
        return;
    }

    GSGLOBAL *gs = GSK_GetGlobal();
    if (!gs)
    {
        return;
    }

    if (pColorMask)
    {
        PROF_ENTER("SNPPUBlendPlanarTo3");
        _PlanarTo3(pInfo->uAttrib8, &pColorMask[0], &pColorMask[1], &pColorMask[2]);
        PROF_LEAVE("SNPPUBlendPlanarTo3");
    }

    PROF_ENTER("SNPPUBlendExec");

    PaletteT *pPal = pInfo->Pal;

    /*
     * 1. Re-upload palette + main8 + sub8 + attrib8 for this line.
     *    The DMA REF points straight at pInfo's EE memory (uncached
     *    alias) so the GIF reads the latest scanline content.
     */
#if SNPPUBLEND_PAL32
    /* CSM1: 16x16 PSMCT32 contiguous CLUT cache. */
    _emit_upload(gs, m_uPalAddr, 1, 0, 0, GS_PSMCT32,
                 (void *)((Uint32)pPal | 0x80000000),
                 16, 16);
#else
    /* CSM2: 256x1 PSMCT16 stripe. */
    _emit_upload(gs, m_uPalAddr, 256, 0, 0, GS_PSMCT16,
                 (void *)((Uint32)pPal | 0x80000000),
                 256, 1);
#endif

    _emit_upload(gs, m_uInputAddr, 256, 0, 0, GS_PSMT8,
                 (void *)((Uint32)pInfo->uMain8 | 0x80000000),
                 256, 1);

    _emit_upload(gs, m_uInputAddr, 256, 0, 1, GS_PSMT8,
                 (void *)((Uint32)pInfo->uSub8 | 0x80000000),
                 256, 1);

    _emit_upload(gs, m_uInputAddr, 256, 0, 2, GS_PSMT8,
                 (void *)((Uint32)pInfo->uAttrib8 | 0x80000000),
                 256, 1);

    /*
     * 2. Switch the destination to the temporary 256x32 PSMCT32
     *    surface used as scratch for the colour-math passes.
     */
    {
        u64 *p = _alloc_ad(gs, 3);
        *p++ = (u64)0;                                                                *p++ = GS_TEXFLUSH;
        *p++ = GS_SETREG_FRAME(m_uTempAddr / 0x20, 256 / 64, GS_PSMCT32, 0);          *p++ = GS_FRAME_1;
        *p++ = GS_SETREG_XYOFFSET(0x8000, 0x8000);                                    *p++ = GS_XYOFFSET_1;
    }

    /*
     * 3. Bind input plane via palette, set blend (Cs * Cs.A + 0) and
     *    push the fixed-colour sprite into temp[1] - sub8 entries
     *    with alpha=0 will pick this colour up.
     */
    Uint32 uFixedColor32 = SNPPUColorConvert15to32(uFixedColor16);
    {
        u64 *p = _alloc_ad(gs, 3);
#if SNPPUBLEND_PAL32
        *p++ = GS_SETREG_TEX0(m_uInputAddr, 256 / 64, GS_PSMT8, 8, 3,
                              1, 0, m_uPalAddr, GS_PSMCT32, 0, 0, 1);                 *p++ = GS_TEX0_1;
#else
        *p++ = GS_SETREG_TEX0(m_uInputAddr, 256 / 64, GS_PSMT8, 8, 3,
                              1, 0, m_uPalAddr, GS_PSMCT16, 1, 0, 1);                 *p++ = GS_TEX0_1;
#endif
        *p++ = GS_SETREG_ALPHA(0, 1, 0, 1, 0x80);                                     *p++ = GS_ALPHA_1;
        *p++ = (u64)uFixedColor32;                                                    *p++ = GS_RGBAQ;
    }

    /* fixed colour -> temp[1] */
    _emit_solid_line(gs, 1, 0);
    /* main8 (palettised) -> temp[0] */
    _emit_tex_line(gs, 0, 0, 0x80808080, 0);
    /* sub8 (palettised), entries with alpha=0 fall through to fixed -> temp[1] */
    _emit_tex_line(gs, 1, 1, 0x80808080, 1);

    /*
     * 4. Mask temp lines through the attribute palettes so that
     *    pixels with the wrong colour-math attribute get zeroed out.
     */
    {
        u64 *p = _alloc_ad(gs, 2);
        *p++ = GS_SETREG_TEX0(m_uInputAddr, 256 / 64, GS_PSMT8, 8, 3,
                              1, 0, m_uAttribMainPal, GS_PSMCT32, 0, 0, 1);           *p++ = GS_TEX0_1;
        *p++ = GS_SETREG_ALPHA(1, 2, 0, 2, 0x20);                                     *p++ = GS_ALPHA_1;
    }
    _emit_tex_line(gs, 0, 2, 0x80808080, 1);

    {
        u64 *p = _alloc_ad(gs, 2);
        *p++ = GS_SETREG_TEX0(m_uInputAddr, 256 / 64, GS_PSMT8, 8, 3,
                              1, 0, m_uAttribSubPal, GS_PSMCT32, 0, 0, 1);            *p++ = GS_TEX0_1;
        *p++ = GS_SETREG_ALPHA(1, 2, 0, 2, 0x80);                                     *p++ = GS_ALPHA_1;
    }
    _emit_tex_line(gs, 1, 2, 0x80808080, 1);

    /*
     * 5. Composite temp[0] (main, masked) and temp[1] (sub, masked or
     *    fixed) into output[iLine]. XYOFFSET shifts the destination Y
     *    by iLine*16 so the line-0 sprite lands on output line iLine.
     *    bAddSub picks add or subtractive blend.
     */
    {
        u64 *p = _alloc_ad(gs, 5);
        *p++ = (u64)0;                                                                *p++ = GS_TEXFLUSH;
        *p++ = GS_SETREG_FRAME(m_uOutAddr / 0x20, 256 / 64, GS_PSMCT32, 0);           *p++ = GS_FRAME_1;
        *p++ = GS_SETREG_TEX0(m_uTempAddr, 256 / 64, GS_PSMCT32, 8, 3,
                              1, 0, 0, 0, 0, 0, 0);                                   *p++ = GS_TEX0_1;
        u64 alpha;
        if (!bAddSub)
        {
            /* (Cs - 0) * Cs.A + Cd  -> additive */
            alpha = GS_SETREG_ALPHA(1, 2, 2, 0, 0x80);
        }
        else
        {
            /* (Cs - Cd) * Cs.A + Cd  -> subtractive (negated) */
            alpha = GS_SETREG_ALPHA(1, 0, 2, 2, 0x80);
        }
        *p++ = alpha;                                                                 *p++ = GS_ALPHA_1;
        *p++ = GS_SETREG_XYOFFSET(0x8000, 0x8000 - (iLine << 4));                     *p++ = GS_XYOFFSET_1;
    }

    /* output[iLine] = main_masked              */
    _emit_tex_line(gs, 0, 0, 0x80808080, 0);
    /* output[iLine] += sub_masked (or fixed)   */
    _emit_tex_line(gs, 0, 1, 0x80808080, 1);

    /*
     * 6. Apply the global intensity (snes "fade") as a multiplicative
     *    pass: output *= (intensity * 0x80 / 15) using a solid-colour
     *    sprite with ALPHA = (Cs - 0) * Cs.A + Cd.
     */
    {
        u64 *p = _alloc_ad(gs, 2);
        *p++ = GS_SETREG_ALPHA(1, 2, 0, 2, 0x80);                                     *p++ = GS_ALPHA_1;
        *p++ = (u64)((uIntensity * 0x80 / 15) << 24);                                 *p++ = GS_RGBAQ;
    }
    _emit_solid_line(gs, 0, 1);

    PROF_LEAVE("SNPPUBlendExec");

    /*
     * 7. Submit and wait. Per-line synchronisation is required because
     *    pInfo->uMain8 / uSub8 / uAttrib8 are scanline-scoped buffers
     *    that the SNES core overwrites the moment Exec returns - if
     *    the DMA were still in flight it would read the next line's
     *    data.
     */
    PROF_ENTER("SNPPUGS");
    gsKit_queue_exec(gs);
    gsKit_finish();
    PROF_LEAVE("SNPPUGS");
}


void SNPPUBlendGS::Clear(SNPPUBlendInfoT *pInfo, Int32 iLine)
{
    /* black scanline */
    Exec(pInfo, iLine, 0, NULL, 0, 0);
}
