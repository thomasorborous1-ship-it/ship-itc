#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "libpad.h"
#include "libxpad.h"
#include "libxmtap.h"
#include "types.h"
#include "input.h"
#include "hw.h"

static char _Input_PadBuf[INPUT_MAXPADS][256]
    __attribute__((aligned(64)))
    __attribute__((section(".bss")));

/* Each entry packs the four 8-bit analog axes reported by libpad in the
   order (rjoy_h, rjoy_v, ljoy_h, ljoy_v) starting from the LSB so that
   0x80808080 is centred. */
static Uint32 _Input_PadData[INPUT_MAXPADS];
static Uint32 _Input_PadAnalog[INPUT_MAXPADS];
static int    _Input_bPadConnected[INPUT_MAXPADS];
static Bool   _Input_bInitialized = FALSE;
static Bool   _Input_bXPad = FALSE;
static Int32  _Input_nPads = 0;

/* Centred-stick value reported by libpad when the controller is digital
   only or when the analog stick is at rest. */
#define INPUT_ANALOG_CENTER  (0x80)

/* Half range of motion that we ignore around the centre. ~37% of the full
   half-range; matches the value used by InfinityStation/uLaunchELF style
   menus and feels comfortable on real DualShock pads, while still being
   loose enough that worn analog sticks register a deflection reliably. */
#define INPUT_ANALOG_DEADZONE (0x30)

static Uint8 _Input_PadPort[INPUT_MAXPADS][2] =
{
    {0, 0},
    {1, 0},
    {1, 1},
    {1, 2},
    {1, 3},
};

static int _Input_GetPadState(int port, int slot)
{
    if (_Input_bXPad)
        return xpadGetState(port, slot);

    return padGetState(port, slot);
}

/* Wait until the pad finishes its libpad initialisation handshake.
 *
 * Original implementation used WaitForNextVRstart(1) between polls,
 * which depends on the iaddis hw.s INTC #3 (VBlank Start) handler
 * incrementing a counter. The gsKit migration installs its own INTC
 * #3 handler via gsKit_add_vsync_handler. Depending on the order in
 * which the two handlers end up registered on the actual chip, the
 * iaddis counter can stop being incremented and WaitForNextVRstart
 * spins forever -- which manifested as boot stalls and as the pad
 * "not responding" on real PS2 hardware while emulators (PCSX2 /
 * NetherSX2) booted fine because their INTC chaining happens to keep
 * both handlers happy.
 *
 * The picodrive PS2 port (irixxxx fork) avoids this whole class of
 * problems by polling padGetState() with a plain usleep() between
 * tries plus a hard timeout, which is independent of any vsync /
 * interrupt handler. Mirror that approach here.
 *
 * Exit conditions:
 *   - PAD_STATE_STABLE   : pad finished init, ready to read
 *   - PAD_STATE_DISCONN  : no pad on this port/slot, give up
 *   - timeout (~5s)      : pad never settled, give up rather than
 *                          freezing the whole boot
 *
 * Note: the previous code also exited on PAD_STATE_FINDCTP1, which is
 * an *intermediate* state ("finding controller type, pass 1") -- the
 * pad is still negotiating. Treating it as "ready" causes subsequent
 * padRead() calls to return zero buttons even with the pad connected,
 * which is consistent with the "menu shows up but controller does not
 * respond" symptom on CRT (Yamark).
 */
static void _Input_WaitPadReady(int port, int slot)
{
    int ret;
    int tm = 50; /* 50 * 100ms = 5s upper bound */

    do
    {
        ret = _Input_GetPadState(port, slot);
        if (ret == PAD_STATE_STABLE || ret == PAD_STATE_DISCONN)
            break;
        usleep(100 * 1000);
    } while (--tm > 0);
}

static int _Input_InitPad(int port, int slot, void *buffer)
{
    int ret;

    if (_Input_bXPad)
    {
        ret = xpadPortOpen(port, slot, buffer);
        if (ret == 0)
        {
            printf("Failed to open xpad port=%d slot=%d\n", port, slot);
            return -1;
        }

        _Input_WaitPadReady(port, slot);
        xpadExitPressMode(port, slot);
        /* Lock the pad in DualShock mode so the analog sticks always
           report deflection, regardless of the user pressing the ANALOG
           button on the controller. Pads that don't support analog mode
           silently keep behaving as digital. */
        xpadSetMainMode(port, slot, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
    }
    else
    {
        ret = padPortOpen(port, slot, buffer);
        if (ret == 0)
        {
            printf("Failed to open pad port=%d slot=%d\n", port, slot);
            return -1;
        }

        _Input_WaitPadReady(port, slot);
        padSetMainMode(port, slot, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
    }

    _Input_WaitPadReady(port, slot);
    return 0;
}

Bool InputIsPadConnected(Uint32 uPad)
{
    if (uPad >= (Uint32)_Input_nPads)
        return FALSE;

    return _Input_bPadConnected[uPad] ? TRUE : FALSE;
}

Uint32 InputGetPadData(Uint32 uPad)
{
    if (!InputIsPadConnected(uPad))
        return 0;

    return _Input_PadData[uPad];
}

Uint32 InputGetPadDpadFromAnalog(Uint32 uPad)
{
    Uint32 packed;
    int    ljoy_h;
    int    ljoy_v;
    Uint32 dpad = 0;

    if (!InputIsPadConnected(uPad))
        return 0;

    packed = _Input_PadAnalog[uPad];
    ljoy_h = (int)((packed >> 16) & 0xff);
    ljoy_v = (int)((packed >> 24) & 0xff);

    /* If the pad is reporting both axes exactly at centre, treat it as a
       digital-only pad and skip the synthesis entirely. This avoids the
       dead-zone test from accidentally emitting d-pad bits when the pad
       has not negotiated DualShock mode. */
    if (ljoy_h == INPUT_ANALOG_CENTER && ljoy_v == INPUT_ANALOG_CENTER)
        return 0;

    if (ljoy_h < (INPUT_ANALOG_CENTER - INPUT_ANALOG_DEADZONE)) dpad |= PAD_LEFT;
    if (ljoy_h > (INPUT_ANALOG_CENTER + INPUT_ANALOG_DEADZONE)) dpad |= PAD_RIGHT;
    if (ljoy_v < (INPUT_ANALOG_CENTER - INPUT_ANALOG_DEADZONE)) dpad |= PAD_UP;
    if (ljoy_v > (INPUT_ANALOG_CENTER + INPUT_ANALOG_DEADZONE)) dpad |= PAD_DOWN;

    return dpad;
}

void InputInit(Bool bXLib)
{
    int iPad;

    _Input_bXPad = bXLib;
    _Input_nPads = bXLib ? INPUT_MAXPADS : 2;

    memset(_Input_PadData, 0, sizeof(_Input_PadData));
    memset(_Input_bPadConnected, 0, sizeof(_Input_bPadConnected));
    memset(_Input_PadBuf, 0, sizeof(_Input_PadBuf));
    for (iPad = 0; iPad < INPUT_MAXPADS; iPad++)
    {
        _Input_PadAnalog[iPad] = 0x80808080U; /* both sticks centred */
    }

    for (iPad = 0; iPad < _Input_nPads; iPad++)
    {
        _Input_InitPad(_Input_PadPort[iPad][0],
                       _Input_PadPort[iPad][1],
                       _Input_PadBuf[iPad]);
    }

    _Input_bInitialized = TRUE;
}

void InputShutdown(void)
{
    int iPad;

    if (!_Input_bInitialized)
        return;

    for (iPad = 0; iPad < _Input_nPads; iPad++)
    {
        if (_Input_bXPad)
            xpadPortClose(_Input_PadPort[iPad][0], _Input_PadPort[iPad][1]);
        else
            padPortClose(_Input_PadPort[iPad][0], _Input_PadPort[iPad][1]);
    }

    memset(_Input_PadData, 0, sizeof(_Input_PadData));
    memset(_Input_bPadConnected, 0, sizeof(_Input_bPadConnected));
    for (iPad = 0; iPad < INPUT_MAXPADS; iPad++)
    {
        _Input_PadAnalog[iPad] = 0x80808080U;
    }

    _Input_nPads = 0;
    _Input_bInitialized = FALSE;
}

void InputPoll(void)
{
    int iPad;

    if (!_Input_bInitialized)
        return;

    for (iPad = 0; iPad < _Input_nPads; iPad++)
    {
        int state;
        Uint32 uData = 0;
        struct padButtonStatus padStatus;

        state = _Input_GetPadState(_Input_PadPort[iPad][0],
                                   _Input_PadPort[iPad][1]);

        /* Only PAD_STATE_STABLE means "ready to read". The previous
           code also accepted PAD_STATE_FINDCTP1, but that is an
           intermediate init state and padRead() returns no useful
           data for it -- which led to the "controller is detected
           but does not respond" symptom on real PS2.

           For freshly inserted pads we no longer call
           WaitForNextVRstart(): it conflicts with the gsKit vsync
           handler on real hardware (same root cause that was already
           patched in the boot path) and is unnecessary here, since
           the next InputPoll() call -- which happens once per frame
           anyway -- will retry. */
        if (state == PAD_STATE_STABLE)
        {
            if (_Input_bPadConnected[iPad] == 0)
            {
                printf("Input: Pad %d inserted!\n", iPad + 1);
            }

            _Input_bPadConnected[iPad] = 1;
        }
        else
        {
            if (_Input_bPadConnected[iPad] == 1)
                printf("Input: Pad %d removed!\n", iPad + 1);

            _Input_bPadConnected[iPad] = 0;
            _Input_PadData[iPad] = 0;
            _Input_PadAnalog[iPad] = 0x80808080U;
            continue;
        }

        memset(&padStatus, 0, sizeof(padStatus));

        if (_Input_bXPad)
            xpadRead(_Input_PadPort[iPad][0], _Input_PadPort[iPad][1], &padStatus);
        else
            padRead(_Input_PadPort[iPad][0], _Input_PadPort[iPad][1], &padStatus);

#ifdef _EE
        uData = 0xffffU ^ (Uint32)padStatus.btns;
#else
        uData = 0;
#endif

        /* Keep _Input_PadData strictly digital so SNES/NES emulation
           never sees synthesised d-pad bits from the analog stick. The
           analog deflection is exposed separately via
           InputGetPadDpadFromAnalog so the menu/UI layer can opt in. */
        _Input_PadData[iPad] = uData;
        _Input_PadAnalog[iPad] =
              ((Uint32)padStatus.ljoy_v << 24)
            | ((Uint32)padStatus.ljoy_h << 16)
            | ((Uint32)padStatus.rjoy_v << 8)
            | ((Uint32)padStatus.rjoy_h);
    }
}
