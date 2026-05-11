
#include <stdio.h>
#include "types.h"
#include "prof.h"
#include "mixbuffer.h"
#include "sjpcmbuffer.h"
#include <string.h>

extern "C" {
#include "sjpcm.h"
};

/* Defined in sjpcm_rpc.c. Writes to EE SIO so the line shows up in
   the emulator log alongside [snes-aud] enq#... entries. */
extern "C" void DLog(const char *fmt, ...);


SJPCMMixBuffer::SJPCMMixBuffer(Uint32 uSampleRate, Bool bAsync)
{
    m_iPrevSample[0]=0;
    m_iPrevSample[1]=0;
    m_nOutSamples = 0;
    m_uSampleRate = uSampleRate;
    m_bAsync      = bAsync;
}


void SJPCMMixBuffer::GetFormat(Uint32 *puSampleRate, Uint32 *pnSampleBits, Uint32 *pnChannels)
{
	*puSampleRate = m_uSampleRate;
	*pnSampleBits = 16;
	*pnChannels   = 2;
}


Int32 SJPCMMixBuffer::GetOutputSamples()
{
    Int32 nSamples;
    Int32 nRaw;

    if (!SjPCM_IsInitialized())
    {
        static int __gos_ni = 0;
        if ((__gos_ni & 0x3F) == 0)
            DLog("[snes-aud] gos: not-init #%d", __gos_ni);
        __gos_ni++;
        return 0;
    }

    PROF_ENTER("SjPCM_Available");

    /*
     * nRaw = free 48 kHz stereo frames the IOP ring buffer can
     * accept.  Capped at 3200 to bound EE CPU load per frame.
     * For 32 kHz the switch below converts to 32 kHz input count.
     */
    nRaw = SjPCM_Available();
    if (nRaw > 4 * 800) nRaw = 4 * 800;
    nRaw &= ~3;
    if (nRaw < 0) nRaw = 0;

    switch (m_uSampleRate)
    {
        case 48000: nSamples = nRaw;                break;
        case 32000: nSamples = (nRaw / 6) * 4;      break;
        case 24000: nSamples = (nRaw / 8) * 4;      break;
        default:    nSamples = 0;                   break;
    }

    {
        static int __gos = 0;
        if ((__gos & 0x3F) == 0)
            DLog("[snes-aud] gos f=%d sr=%u avail=%d out=%d async=%d",
                 __gos, (unsigned)m_uSampleRate,
                 (int)nRaw, (int)nSamples, (int)m_bAsync);
        __gos++;
    }

    PROF_LEAVE("SjPCM_Available");

    m_uLastOutput  = nSamples;
    return nSamples;
}

static inline Int16 clamp16(Int32 v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (Int16)v;
}

Int32 SJPCMMixBuffer::ConvertSamples2to3(Int16 *pOut, Int16 *pIn, Int32 nSamples, Int32 *pPrevSample)
{
    /*
     * Cubic Hermite 2:3 upsampler  (32 kHz -> 48 kHz).
     *
     * For every 2 input samples we emit 3 output samples.
     * The output positions in terms of the input grid are:
     *   out[0] = input position 0        (exact copy of s0)
     *   out[1] = input position 2/3      (between s0 and s1)
     *   out[2] = input position 4/3      (= 1/3 between s1 and s2)
     *
     * Expanding the Hermite basis with Catmull-Rom tangent
     * estimates  m_k = (s_{k+1} - s_{k-1}) / 2,  the four-sample
     * weights reduce to:
     *
     *   out[1] = (-1/27)*s_m1 + (9/27)*s0 + (21/27)*s1 + (-2/27)*s2
     *   out[2] = (-2/27)*s0  + (21/27)*s1 + (9/27)*s2  + (-1/27)*s3
     *
     * Coefficients use 12-bit fixed point (shift 12) to stay safely
     * within 32-bit multiply range on the EE.
     */
    Int16 *pOutStart = pOut;
    Int32 s_m1 = pPrevSample[0];

    while (nSamples >= 2)
    {
        Int32 s0 = pIn[0];
        Int32 s1 = pIn[1];
        Int32 s2 = (nSamples >= 4) ? (Int32)pIn[2] : s1;
        Int32 s3 = (nSamples >= 6) ? (Int32)pIn[3] : s2;

        pOut[0] = (Int16)s0;

        pOut[1] = clamp16((-152 * s_m1 + 1365 * s0
                          + 3186 * s1 - 303 * s2) >> 12);

        pOut[2] = clamp16((-303 * s0 + 3186 * s1
                          + 1365 * s2 - 152 * s3) >> 12);

        s_m1 = s1;
        pIn  += 2;
        pOut += 3;
        nSamples -= 2;
    }

    pPrevSample[0] = s_m1;
    return (Int32)(pOut - pOutStart);
}


Int32 SJPCMMixBuffer::ConvertSamplesStereo_32000(Int16 *pLeftSamples, Int16 *pRightSamples, Int16 *pOutLeft, Int16 *pOutRight, Int32 nInSamples)
{
    Int32 nOutSamples;

    if (nInSamples > SJPCMMIXBUFFER_MAXENQUEUE*2/3) nInSamples = SJPCMMIXBUFFER_MAXENQUEUE*2/3;

    PROF_ENTER("SjPCM_Convert");
    ConvertSamples2to3(pOutLeft, pLeftSamples, nInSamples, &m_iPrevSample[0]);
    nOutSamples=ConvertSamples2to3(pOutRight, pRightSamples, nInSamples, &m_iPrevSample[1]);
    PROF_LEAVE("SjPCM_Convert");

    return nOutSamples;
}

void SJPCMMixBuffer::OutputSamplesStereo(Int16 *pLeftSamples, Int16 *pRightSamples, Int32 nSamples)
{
    Int16 *pOutLeft, *pOutRight;
    Int32 nOutSamples;

    // determine output space required (estimate)
    switch (m_uSampleRate)
    {
        case 24000:
            nOutSamples = nSamples * 2;
            break;
        case 32000:
            nOutSamples = nSamples * 6 / 4;
            break;
        default:
        case 48000:
            nOutSamples = nSamples;
            break;
    }

    {
        static int __oss = 0;
        if ((__oss & 0x3F) == 0)
            DLog("[snes-aud] oss f=%d in=%d est=%d acc=%d max=%d",
                 __oss, (int)nSamples, (int)nOutSamples,
                 (int)m_nOutSamples, (int)SJPCMMIXBUFFER_MAXENQUEUE);
        __oss++;
    }

    // check for buffer overflow 
    if ((m_nOutSamples + nOutSamples) > SJPCMMIXBUFFER_MAXENQUEUE)
    {
        return;
    }

    // buffer samples locally
    pOutLeft    = m_OutData[0] + m_nOutSamples;
    pOutRight   = m_OutData[1] + m_nOutSamples;

    switch(m_uSampleRate)
    {
        default:
        case 24000:
        case 48000:
            memcpy(pOutLeft, pLeftSamples, nSamples * sizeof(Int16));
            memcpy(pOutRight, pRightSamples, nSamples * sizeof(Int16));
            m_nOutSamples += nSamples;
            break;
        case 32000:
            m_nOutSamples += ConvertSamplesStereo_32000(pLeftSamples, pRightSamples, pOutLeft, pOutRight, nSamples);
            break;
    }
}

void SJPCMMixBuffer::Flush()
{
    Int32 nOutSamples;

    nOutSamples = m_nOutSamples;

    {
        static int __fc = 0;
        if ((__fc & 0x3F) == 0)
        {
            DLog("[snes-aud] flush f=%d nout=%d async=%d",
                 __fc, (int)nOutSamples, (int)m_bAsync);
        }
        __fc++;
    }

    if (nOutSamples > 0)
    {
        if (nOutSamples & 1)
        {
            // uh oh
            #if CODE_DEBUG
            printf("Sample count not even! %d\n", nOutSamples);
            #endif
            nOutSamples &= ~1;
        }

        if (nOutSamples > SJPCMMIXBUFFER_MAXENQUEUE)
        {
            // uh oh
            #if CODE_DEBUG
            printf("Sample buffer overflow! %d\n", nOutSamples);
            #endif
            nOutSamples = SJPCMMIXBUFFER_MAXENQUEUE;
        }


        if (m_bAsync)
        {
            SjPCM_EnqueueAsync(m_OutData[0], m_OutData[1], nOutSamples);
        } else
        {
            SjPCM_Enqueue(m_OutData[0], m_OutData[1], nOutSamples,1);
        }
    }

    m_nOutSamples = 0;
}



void SJPCMMixBuffer::OutputSamplesMono(Int16 *pSamples,Int32 nSamples)
{
    OutputSamplesStereo(pSamples, pSamples, nSamples);
}


