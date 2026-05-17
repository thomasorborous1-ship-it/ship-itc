/* nessystem.h
 *
 * Emu::System wrapper that owns the InfoNES NES core for the PS2 build.
 *
 * Mirrors the structure of SnesSystem (snes.h) so the mainloop's polymorphic
 * dispatch through Emu::System* works identically for NES and SNES.
 *
 * Phase 2 scope:
 *   - SetRom() stores the NesRom* (does NOT yet hand the ROM data to InfoNES).
 *   - Reset()  is a no-op (real InfoNES_Reset() in Phase 3).
 *   - ExecuteFrame() paints the render surface a solid colour ("Mario isn't
 *     running yet, but at least we know our code is being dispatched") and
 *     advances the frame counter. This is intentional - we want a clear
 *     visual signal in NetherSX2 that the NES branch is reached.
 *   - SaveState / RestoreState are stubs.
 *   - GetString returns the correct file extensions for SRAM and save state
 *     so the rest of the mainloop's path-building logic works.
 *
 * Phase 3 fills in real ExecuteFrame using the InfoNES rendering pipeline.
 */

#ifndef _NESSYSTEM_H
#define _NESSYSTEM_H

#include "emusys.h"

#include "nesrom.h"
#include "nesstate.h"


class NesMMU; /* Phase 5 - FDS disk-swap mux. Forward-declared so other
                 code can reference NesSystem::GetMMU() without dragging
                 the full type in until we need it. */


class NesSystem : public Emu::System
{
public:
    NesSystem();
    ~NesSystem();

    /* Emu::System hooks ---------------------------------------------- */
    virtual void  SetRom(Emu::Rom *pRom);
    virtual void  Reset();
    virtual void  SoftReset();

    virtual void  ExecuteFrame(Emu::SysInputT *pInput,
                               class CRenderSurface *pTarget,
                               class CMixBuffer *pMixBuf,
                               ModeE eMode);

    virtual Int32 GetStateSize();
    virtual void  SaveState(void *pState, Int32 nStateBytes);
    virtual void  RestoreState(void *pState, Int32 nStateBytes);

    virtual Int32  GetSRAMBytes();
    virtual Uint8 *GetSRAMData();

    virtual const char *GetString(StringE eString);
    virtual Uint32 GetSampleRate();

    /* NES-only state hooks used by mainloop_state.cpp once it's
       un-#if-0'd. They mirror SnesSystem::Save/RestoreState. */
    void          SaveState(NesStateT *pState);
    Bool          RestoreState(NesStateT *pState);

    /* FDS disk swapping (Phase 5). The current build returns NULL so
       mainloop_input.cpp's FDS swap branch is a no-op even after the
       #if 0 around it is flipped. */
    NesMMU       *GetMMU()                  {return NULL;}
    void          SetNesDisk(NesDisk *pDisk) {m_pNesDisk = pDisk;}

private:
    NesRom    *m_pNesRom;     /* current cartridge image, owned by mainloop */
    NesDisk   *m_pNesDisk;    /* current FDS disk     (Phase 5) */
    Uint32     m_uFrameTick;  /* per-frame counter, used by the stub
                                 paint to flicker so the user can see
                                 the loop is alive */
};

#endif
