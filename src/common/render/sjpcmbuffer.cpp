
#include <stdio.h>
#include "types.h"
#include "prof.h"
#include "mixbuffer.h"
#include "sjpcmbuffer.h"
#include <string.h>

extern "C" {
#include "sjpcm.h"
};


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
        // we're not initialized
        return 0;
    }

    PROF_ENTER("SjPCM_Available");

    /*
     * Ask the audsrv backend how many sample-frames the IOP ring
     * buffer can accept right now.  This replaces the old formula
     *     nSamples = 4 * 800 - SjPCM_Buffered();
     * which assumed the ring buffer held exactly 3200 frames.
     * audsrv uses a 20480-byte (5120-frame) ring, and
     * audsrv_queued() can report a non-zero initial occupancy
     * even before any audio is enqueued, so the old formula
     * chronically under-produced audio (~424 samples/frame
     * instead of the ~533 needed at 32 kHz / 60 fps).
     *
     * SjPCM_Available() -> audsrv_available() / 4  gives the
     * real free space.  We cap at 3200 so a single frame never
     * tries to mix more than the old worst-case, keeping EE CPU
     * load bounded.
     */
    nRaw = SjPCM_Available();
    if (nRaw > 4 * 800) nRaw = 4 * 800;
    nRaw &= ~3;
    if (nRaw < 0) nRaw = 0;

    // determine number of samples needed for input
    switch (m_uSampleRate)
    {
        case 48000:
            // sample output is 1:1
            nSamples = nRaw;
            break;
        case 32000:
            // sample output is 2:3
            // this must be divisible by 4 so that the output count is even
            nSamples = (nRaw / 6) * 4;
            break;
        case 24000:
            // sample output is 1:2
            // this must be divisible by 4 so that the output count is even
            nSamples = (nRaw / 8) * 4;
            break;
        default:
            nSamples = 0;
    }

    PROF_LEAVE("SjPCM_Available");

	m_uLastOutput  = nSamples;

	return nSamples;
}

Int32 SJPCMMixBuffer::ConvertSamples2to3(Int16 *pOut, Int16 *pIn, Int32 nSamples, Int32 *pPrevSample)
{
    Int32 iSample0, iSample1, iSample2;
    Int32 TwoThird = 0x10000 * 2 / 3;
    Int32 OneThird = 0x10000  - TwoThird;
    Int16 *pOutStart = pOut;
//      Int16 *pInStart = pIn;

    // for every two input samples, output 3 output samples...
    // 96hz xxxxxxxxxxxxxxxxxxxxxxxxxxx
    // 48hz x-x-x-x-x-x-x-x-x-x-x-x-x-x
    // 32hz x--x--x--x--x--x--x--x--x--

    iSample0 = *pPrevSample;

    //
    while (nSamples > 0)
    {
        iSample1 = pIn[0];
        iSample2 = pIn[1];

        pOut[0] = iSample0;
        pOut[1] = (iSample0 * OneThird + iSample1 * TwoThird) >> 16;
        pOut[2] = (iSample1 * TwoThird + iSample2 * OneThird) >> 16;

        iSample0 = iSample2;

        pIn+=2;
        pOut+=3;
        nSamples-=2;
    }

    *pPrevSample = iSample0;

//        printf("%d %d\n", pOut- pOutStart, pIn - pInStart);
    return pOut - pOutStart;
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
        case 32000:
            m_nOutSamples += ConvertSamplesStereo_32000(pLeftSamples, pRightSamples, pOutLeft, pOutRight, nSamples);
            break;

        default:
        case 24000:
        case 48000:
            // leave data as is
            memcpy(pOutLeft, pLeftSamples, nSamples * sizeof(Int16));
            memcpy(pOutRight, pRightSamples, nSamples * sizeof(Int16));
            m_nOutSamples += nSamples;
            break;
    }
}

void SJPCMMixBuffer::Flush()
{
    Int32 nOutSamples;

    nOutSamples = m_nOutSamples;

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


