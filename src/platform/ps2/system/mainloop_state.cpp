#include <stdio.h>
#include <string.h>

#include "types.h"
#include "console.h"
#include "file.h"
#include "prof.h"
#include "memcard.h"
#include "mainloop_debug.h"

extern "C" {
int MCSave_Write(char *pPath, char *pData, int nBytes);
int MCSave_WriteSync(int block, int *pResult);
}

#include "mainloop_shared.h"
#include "mainloop_state.h"

/* MCSAVE.IRX (custom async memory-card writer) is intentionally NOT
   embedded in the ELF -- see Makefile lines ~130-154 and
   embedded_irx.cpp. On NetherSX2 and on PS2 setups where MCSAVE.IRX
   isn't shipped next to the binary, IOPLoadModule("MCSAVE.IRX")
   fails and this flag stays FALSE. MCSave_Write then silently
   returns 0 because _MCSave_nBufferBytes is still 0 (no Init), so
   _MainLoopSaveSRAM(TRUE) returns FALSE and the menu modal shows
   "Error Saving SRAM!". The save path below falls back to the
   synchronous libmc-via-fioWrite API in that case -- the same one
   _MainLoopLoadSRAM already uses for reads. Defined in
   mainloop_iop.cpp. */
extern Bool _MainLoop_bMCSaveReady;

#if MAINLOOP_HISTORY
extern Uint32 _nHistory;
#endif


/* MAINLOOP_STATEPATH lives in mainloop_shared.h (included above). */

static Uint32 _PathCalcHash(const char *pStr)
{
    Uint32 hash = 0;

    while (*pStr)
    {
        hash *= 33;
        hash += *pStr;
        pStr++;
    }

    return hash;
}

void PathTruncFileName(Char *pOut, Char *pStr, Int32 nMaxChars)
{
    Uint32 hash;

    hash = _PathCalcHash(pStr);

    // copy string up to maxchars length
    while (*pStr && nMaxChars > 0)
    {
        *pOut++ = *pStr++;
        nMaxChars--;
    }

    // terminate
    *pOut = 0;

    if (nMaxChars <= 0)
    {
        // mangle end of name
        sprintf(pOut - 3, "%03d", hash % 1000);
    }
}

int PathGetMaxFileNameLength(const char *pPath)
{
    if (pPath[0] == 'm' && pPath[1] == 'c')
    {
        return 32;
    }

    return 256;
}

static Uint32 _CalcChecksum(Uint32 *pData, Uint32 nWords)
{
    Uint32 uSum = 0;

    while (nWords > 0)
    {
        uSum += pData[0];
        pData++;
        nWords--;
    }

    return uSum;
}

Bool _MainLoopHasSRAM()
{
    return _pSystem ? (_pSystem->GetSRAMBytes() > 0) : FALSE;
}

Bool _MainLoopSaveSRAM(Bool bSync)
{
    Int32 nSramBytes = _pSystem ? _pSystem->GetSRAMBytes() : 0;

    if (nSramBytes > 0)
    {
        Char Path[1024];
        Char SaveName[256];
        Uint8 *pSRAM;

        pSRAM = _pSystem->GetSRAMData();

        {
            static Bool bDirEnsured = FALSE;
            if (!bDirEnsured)
            {
                int rc = MemCardCreateSave(_SramPath, _MainLoop_SaveTitle, TRUE);
                printf("[SRAM] lazy MemCardCreateSave('%s') -> %d\n", _SramPath, rc);
                bDirEnsured = TRUE;
            }
        }

        PathTruncFileName(SaveName, _RomName, PathGetMaxFileNameLength(_SramPath) - 4);
        snprintf(
            Path,
            sizeof(Path),
            "%s/%s.%s",
            _SramPath,
            SaveName,
            _pSystem->GetString(Emu::System::StringE::STRING_SRAMEXT)
        );

        ML_TRACE("SRAM save begin: rom='%s' bytes=%d sync=%d", _RomName, (int)nSramBytes, (int)bSync);
        ML_TRACE("SRAM save path: %s", Path);

        if (_MainLoop_bMCSaveReady)
        {
            /* Async path via the custom MCSAVE.IRX RPC server. Only
               reachable when the IRX actually loaded (real PS2 with
               the file shipped next to the ELF). */
            MCSave_WriteSync(TRUE, NULL);
            MCSave_Write((char *)Path, (char *)pSRAM, nSramBytes);

            if (bSync)
            {
                int result;

                MCSave_WriteSync(TRUE, &result);
                ML_TRACE("SRAM save sync result: %d", result);
                return result ? TRUE : FALSE;
            }

            return TRUE;
        }
        else
        {
            /* Sync fallback for NetherSX2 / any setup where
               MCSAVE.IRX failed to load. Goes through rom0:XMCMAN
               + rom0:XMCSERV via fileio's mc: device binding -- the
               same path _MainLoopLoadSRAM uses for reads and the
               same path MemCardCreateSave used at boot to write
               icon.sys / icon.icn into mc0:/SNESticle/, so if the
               save directory exists at all on the card then this
               write will reach it. */
            Bool bOk = MemCardWriteFile(Path, pSRAM, nSramBytes);
            ML_TRACE("SRAM save (memcard fallback): %d", (int)bOk);
            return bOk;
        }
    }

    ML_TRACE("SRAM save skipped: no SRAM");
    return FALSE;
}

void _MainLoopLoadSRAM()
{
    Int32 nSramBytes = _pSystem ? _pSystem->GetSRAMBytes() : 0;

    if (nSramBytes > 0)
    {
        Char Path[1024];
        Char SaveName[256];
        Uint8 *pSRAM;

        pSRAM = _pSystem->GetSRAMData();

        PathTruncFileName(SaveName, _RomName, PathGetMaxFileNameLength(_SramPath) - 4);
        snprintf(
            Path,
            sizeof(Path),
            "%s/%s.%s",
            _SramPath,
            SaveName,
            _pSystem->GetString(Emu::System::StringE::STRING_SRAMEXT)
        );

        ML_TRACE("SRAM load begin: rom='%s' bytes=%d", _RomName, (int)nSramBytes);
        ML_TRACE("SRAM load path: %s", Path);

        if (MemCardReadFile(Path, pSRAM, nSramBytes))
        {
            _MainLoop_SRAMChecksum = _CalcChecksum((Uint32 *)pSRAM, nSramBytes / 4);
            ConPrint("SRAM loaded: %s\n", Path);
            ML_TRACE("SRAM load checksum: %08X", (unsigned int)_MainLoop_SRAMChecksum);
        }
        else
        {
            ML_TRACE("SRAM load failed or file missing: %s", Path);
        }

        _MainLoop_SRAMUpdated = FALSE;
    }

    _MainLoop_SaveCounter = 0;
    _bStateSaved = FALSE;
}

Bool _MainLoopCheckSRAM()
{
    Int32 nSramBytes = _pSystem ? _pSystem->GetSRAMBytes() : 0;

    if (nSramBytes > 0)
    {
        Uint8 *pSRAM = _pSystem->GetSRAMData();
        Uint32 uChecksum;

        PROF_ENTER("_MainLoopCheckSRAM");

        uChecksum = _CalcChecksum((Uint32 *)pSRAM, nSramBytes / 4);

        if (_MainLoop_SRAMChecksum != uChecksum)
        {
#if CODE_DEBUG
            printf("SRAM changed!\n");
#endif
            ML_TRACE(
                "SRAM checksum changed: old=%08X new=%08X",
                (unsigned int)_MainLoop_SRAMChecksum,
                (unsigned int)uChecksum
            );

            _MainLoop_SRAMUpdated = TRUE;
            _MainLoop_SaveCounter = _MainLoop_AutoSaveTime;
            _MainLoop_SRAMChecksum = uChecksum;
        }

        if (_MainLoop_SaveCounter > 0)
        {
            _MainLoop_SaveCounter--;

            if (_MainLoop_SaveCounter == 0)
            {
                ML_TRACE("SRAM autosave trigger");
                _MainLoopSaveSRAM(FALSE);
            }
        }

        PROF_LEAVE("_MainLoopCheckSRAM");
    }

    return TRUE;
}

void _MainLoopLoadState()
{
    Char Path[1024];

    /*
    printf("%d\n", sizeof(_SnesState));
    printf("SNStateCPUT %d\n",sizeof(SNStateCPUT ));
    printf("SNStatePPUT %d\n",sizeof(SNStatePPUT ));
    printf("SNStateIOT %d\n",sizeof(SNStateIOT ));
    printf("SNStateDMACT %d\n",sizeof(SNStateDMACT ));
    printf("SNStateSPCT %d\n",sizeof(SNStateSPCT ));
    printf("SNStateSPCDSPT %d\n",sizeof(SNStateSPCDSPT ));
    */

    if (!_pSystem)
        return;

    if (_pSystem == _pSnes)
    {
        snprintf(Path, sizeof(Path), "%s%s.sns", MAINLOOP_STATEPATH, _RomName);
        ML_TRACE("State load path: %s", Path);

        if (FileReadMem(Path, &_SnesState, sizeof(_SnesState)))
        {
            _bStateSaved = TRUE;
            ConPrint("State loaded from %s\n", Path);
            ML_TRACE("State load ok");
        }
        else
        {
            ML_TRACE("State load failed or file missing");
        }

        if (_bStateSaved)
        {
            _pSnes->RestoreState(&_SnesState);
            ML_TRACE("State restore applied");
        }
    }

#if 0
    else if (_pSystem == _pNes)
    {
        sprintf(Path, "%s%s.nst", MAINLOOP_STATEPATH, _RomName);

        if (FileReadMem(Path, &_NesState, sizeof(_NesState)))
        {
            _bStateSaved = TRUE;
            ConPrint("State loaded from %s\n", Path);
        }

        if (_bStateSaved)
        {
            _pNes->RestoreState(&_NesState);
        }
    }
#endif
}

void _MainLoopSaveState()
{
    Char Path[1024];

    if (!_pSystem)
        return;

    if (_pSystem == _pSnes)
    {
        snprintf(Path, sizeof(Path), "%s%s.sns", MAINLOOP_STATEPATH, _RomName);
        ML_TRACE("State save path: %s", Path);

        _pSnes->SaveState(&_SnesState);
        _bStateSaved = TRUE;

        if (FileWriteMem(Path, &_SnesState, sizeof(_SnesState)))
        {
            ConPrint("State saved to %s\n", Path);
            ML_TRACE("State save ok");
        }
        else
        {
            ML_TRACE("State save failed");
        }
    }

#if 0
    else if (_pSystem == _pNes)
    {
        sprintf(Path, "%s%s.nst", MAINLOOP_STATEPATH, _RomName);

        _pNes->SaveState(&_NesState);
        _bStateSaved = TRUE;

        if (FileWriteMem(Path, &_NesState, sizeof(_NesState)))
        {
            ConPrint("State saved to %s\n", Path);
        }
    }
#endif
}


void _MainLoopResetHistory()
{
#if MAINLOOP_HISTORY
    _nHistory = 0;
#endif
}


void _MainLoopResetInputChecksums()
{
	_uInputFrame =0;
	memset(_uInputChecksum, 0, sizeof(_uInputChecksum));
}

#if MAINLOOP_HISTORY
Uint32 _History[16384 * 2];
Uint32 _nHistory = 0;
#endif

#if MAINLOOP_HISTORY

void _MainLoopSaveHistory()
{
    FileWriteMem("host:game.hst", _History, _nHistory * sizeof(Uint32));
    printf("History written\n");
}
#endif
