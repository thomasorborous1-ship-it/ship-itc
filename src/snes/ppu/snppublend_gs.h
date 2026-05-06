
#ifndef _SNPPUBLEND_GS_H
#define _SNPPUBLEND_GS_H


#include "snppublend.h"

/*
 * Per-render-surface state for the gsKit-based blender. The legacy
 * SNPPUDmaListT struct that owned a 2KB pre-baked GIF chain plus four
 * patch pointers no longer exists - the chain is rebuilt in gsKit's
 * drawbuffer heap on every Exec() call.
 */
struct SNPPUBlendColorCalibT
{
	Float32	y_mul,y_add;
	Float32	i_mul,i_add;
	Float32	q_mul,q_add;
};



class SNPPUBlendGS : public ISNPPUBlend
{
    /* VRAM TBP addresses (256-byte units). Set up once by the
     * constructor and never modified afterwards - the chain reads
     * them as immediate values. */
    Uint32      m_uPalAddr;
    Uint32      m_uInputAddr;
    Uint32      m_uAttribMainPal;
    Uint32      m_uAttribSubPal;
    Uint32      m_uTempAddr;
    Uint32      m_uOutAddr;

    class CRenderSurface *m_pTarget;

public:
    SNPPUBlendGS(Uint32 uVramAddr, Uint32 uOutAddr);

    virtual void Begin(class CRenderSurface *pTarget);
    virtual void Exec(SNPPUBlendInfoT *pInfo, Int32 iLine, Uint32 uFixedColor32, SNMaskT *pColorMask, Bool bAddSub, Uint32 uIntensity);
    virtual void Clear(SNPPUBlendInfoT *pInfo, Int32 iLine);
    virtual void End();
    virtual void UpdatePalette(SNPPUBlendInfoT *pInfo, Uint16 *pCGRam, Uint32 uIntensity);
    virtual void UpdatePaletteEntry(SNPPUBlendInfoT *pInfo, Uint32 uAddr, Uint32 uData, Uint32 uIntensity);

	static void ColorCalibrate(SNPPUBlendColorCalibT *pCalib);
};



#endif
