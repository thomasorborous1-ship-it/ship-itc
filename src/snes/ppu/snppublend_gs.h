
#ifndef _SNPPUBLEND_GS_H
#define _SNPPUBLEND_GS_H


#include "snppublend.h"

struct SNPPUDmaListT
{
    /* Chain buffer, allocated once from gsKit's UCAB pool in the
       SNPPUBlendGS constructor. Holds 128 quadwords of GIF tags,
       DMA tags, and the per-scanline patch slots that the blender
       rewrites every Exec(). UCAB is uncached + write-combined, so
       the EE writes go straight to physical RAM and no FlushCache
       is required before kicking the chain on the GIF channel. */
    Uint128     *Data;

    Uint64      *pFixedColor;
    Uint64      *pAddSub;
    Uint64      *pIntensity;
    Uint64      *pXYOffset;

    Uint32      uPalAddr;
    Uint32      uInputAddr;
    Uint32      uAttribMainPal;
    Uint32      uAttribSubPal;
    Uint32      uTempAddr;

	Uint32		uOutAddr;
};

/* Number of quadwords the blender's UCAB chain buffer holds. */
#define SNPPUBLEND_CHAIN_QWORDS 128

struct SNPPUBlendColorCalibT
{
	Float32	y_mul,y_add;
	Float32	i_mul,i_add;
	Float32	q_mul,q_add;
};



class SNPPUBlendGS : public ISNPPUBlend
{
    SNPPUDmaListT m_DmaList _ALIGN(16);
    SNPPUBlendInfoT *m_pDmaBlendInfo;

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
