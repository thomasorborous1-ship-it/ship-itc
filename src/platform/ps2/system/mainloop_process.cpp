/* mainloop_process.cpp
 *
 * Hosts MainLoopProcess() -- the per-frame driver that polls input,
 * runs the SNES core, drives netplay/movie clip, kicks SRAM bookkeeping
 * and finally calls into MainLoopRender().
 *
 * Owns the file-static _iframetex flip flag used to alternate between
 * the two CRenderSurface entries in _fbTexture[].
 *
 * Extracted from mainloop.cpp during the Batch 3 split. Behaviour is
 * unchanged.
 */

#include <stdio.h>

#include "mainloop_debug.h"
#include "mainloop_shared.h"
#include "mainloop_input.h"
#include "mainloop_state.h"
#include "mainloop_exec.h"

#include "types.h"
#include "console.h"
#include "input.h"
#include "snes.h"
#include "rendersurface.h"
#include "mixbuffer.h"
#include "prof.h"
#include "emusys.h"
#include "emumovie.h"

extern "C" {
#include "gpprim.h"
};

extern "C" {
#include "netplay_ee.h"
};

extern "C" {
#include "sjpcm.h"
};


static Uint32 _iframetex=0;


Bool MainLoopProcess()
{
    { static int __d=0; if(__d<10){ printf("[diag] f=%d menu=%d blk=%d scr=%p\n",__d,(int)_bMenu,(int)_MainLoop_BlackScreen,(void*)_MainLoop_pScreen); __d++; } }
    NetPlayRPCInputT NetInput;

    PROF_ENTER("Frame");

    PROF_ENTER("NetPlayRPCProcess");
    NetPlayRPCProcess();
    PROF_LEAVE("NetPlayRPCProcess");

    PROF_ENTER("InputProcess");
    InputPoll();

    PROF_LEAVE("InputProcess");


	_MainLoopInputProcess(InputGetPadData(0) | InputGetPadData(1) | InputGetPadData(2) | InputGetPadData(3));

//	_MainLoopInputProcess(InputGetPadData(0));
//	_MainLoopInputProcess(InputGetPadData(1));

    if (!_bMenu && _pSystem && !_MainLoop_BlackScreen)
    {
        CRenderSurface *pSurface;
        CMixBuffer *pMixBuffer = NULL;
        pSurface = _fbTexture[_iframetex];
	
		Emu::SysInputT Input;
	
		Int32 iPad;

        /*
        if (_WavFile.IsOpen())
        {
            pMixBuffer = &_WavFile;
        } else
        {
            pMixBuffer = &_SJPCMMix;
        } 
        */                        
        pMixBuffer = _SJPCMMix;
//                pMixBuffer = NULL;

		// read inputs
		for (iPad=0; iPad < 5; iPad++)
		{
			if (InputIsPadConnected(iPad))
			{
				Input.uPad[iPad] = _MainLoopInput(InputGetPadData(iPad));
			} else
			{
				Input.uPad[iPad] = EMUSYS_DEVICE_DISCONNECTED;
			}
		}

		// send controller 1 + 2 inputs combined to 32-bits
		NetInput.InputSend = ((Uint32)Input.uPad[0]) | (((Uint32)Input.uPad[1])<<16);

        PROF_ENTER("NetPlayClientInput");
        NetPlayClientInput(&NetInput);
        PROF_LEAVE("NetPlayClientInput");

        if (NetInput.eGameState == NETPLAY_GAMESTATE_PLAY)
        {
            if ((_pSystem->GetFrame()+1) != NetInput.uFrame)
            {
				#if CODE_DEBUG
                printf("Not executing frame %d %d\n", NetInput.uFrame, _pSystem->GetFrame());
				#endif
                NetInput.eGameState = NETPLAY_GAMESTATE_PAUSE;
            }

			// we are connected, retrieve input data
	        Input.uPad[0] = (Uint16)NetInput.InputRecv[0];
	        Input.uPad[1] = (Uint16)NetInput.InputRecv[1];
	        Input.uPad[2] = (Uint16)NetInput.InputRecv[2];
	        Input.uPad[3] = (Uint16)NetInput.InputRecv[3];
			Input.uPad[4] = EMUSYS_DEVICE_DISCONNECTED;

			if (Input.uPad[2] == EMUSYS_DEVICE_DISCONNECTED)
			{
				// if controller 3 is disconnected, use controller 2 of first peer
				Input.uPad[2] = (Uint16)(NetInput.InputRecv[0]>>16);
			}

			if (Input.uPad[3] == EMUSYS_DEVICE_DISCONNECTED)
			{
				// if controller 4 is disconnected, use controller 2 of second peer
				Input.uPad[3] = (Uint16)(NetInput.InputRecv[1]>>16);
			}
		
        }  
		else
		{
		
            if (s_pMovieClip->IsPlaying())
            {
                if (!s_pMovieClip->PlayFrame(Input))
                {
                    s_pMovieClip->PlayEnd();
                    ConPrint("Movie: Play End\n");
                }
            }
	
		}

        if (NetInput.eGameState != NETPLAY_GAMESTATE_PAUSE)
        {
			Emu::System::ModeE eMode;

            #if MAINLOOP_HISTORY
            if (_nHistory < 16384 * 2)
            {
                _History[_nHistory++] = Input.uPad[0];
                _History[_nHistory++] = Input.uPad[1];
                _History[_nHistory++] = Input.uPad[2];
                _History[_nHistory++] = Input.uPad[3];
            }
            #endif

			_uInputFrame    = NetInput.uFrame;
			_uInputChecksum[0] += Input.uPad[0];
			_uInputChecksum[1] += Input.uPad[1];
			_uInputChecksum[2] += Input.uPad[2];
			_uInputChecksum[3] += Input.uPad[3];
			_uInputChecksum[4] += Input.uPad[4];

			eMode = (NetInput.eGameState == NETPLAY_GAMESTATE_IDLE) ? Emu::System::MODE_ACCURATENONDETERMINISTIC : Emu::System::MODE_INACCURATEDETERMINISTIC;

            if (s_pMovieClip->IsRecording())
            {
                if (!s_pMovieClip->RecordFrame(Input))
                {
                    s_pMovieClip->RecordEnd();
                    ConPrint("Movie: Reached end of record buffer!\n");
                }
            }

#if 0
            if (_pSystem==_pSnes)
            {
//                _ExecuteSnes(NULL, NULL, &Input, eMode);
 			    GPPrimDisableZBuf();
                _ExecuteSnes(pSurface, pMixBuffer, &Input, eMode);
//                TextureUpload(&_OutTex, pSurface->GetLinePtr(0));
            } else
            if (_pSystem==_pNes)
            {
                _ExecuteNes(pSurface, pMixBuffer, &Input, eMode);

                TextureUpload(&_OutTex, pSurface->GetLinePtr(0));
            } 
#else
 			    GPPrimDisableZBuf();
                _ExecuteSnes(pSurface, pMixBuffer, &Input, eMode);
#endif
		    _iframetex^=1;
        }

        SjPCM_BufferedAsyncStart();
    }

    _MainLoopCheckSRAM();

	MainLoopRender();

    PROF_LEAVE("Frame");

    #if PROF_ENABLED
    ProfProcess();
    #endif

    return TRUE;
}
