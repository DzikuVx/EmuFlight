/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Created by jflyper */

#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>

#include "platform.h"

#ifdef VTX_TRAMP

#include "build/debug.h"

#include "common/utils.h"
#include "common/printf.h"

#include "io/serial.h"
#include "drivers/serial.h"
#include "drivers/vtx_var.h"
#include "drivers/system.h"
#include "io/vtx_tramp.h"
#include "io/vtx_common.h"

static serialPort_t *trampSerialPort = NULL;

static uint8_t trampReqBuffer[16];
static uint8_t trampRespBuffer[16];

typedef enum {
    TRAMP_STATUS_BAD_DEVICE = -1,
    TRAMP_STATUS_OFFLINE = 0,
    TRAMP_STATUS_ONLINE,
    TRAMP_STATUS_SET_FREQ_PW,
    TRAMP_STATUS_CHECK_FREQ_PW
} trampStatus_e;

trampStatus_e trampStatus = TRAMP_STATUS_OFFLINE;

uint32_t trampRFFreqMin;
uint32_t trampRFFreqMax;
uint32_t trampRFPowerMax;

uint32_t trampCurFreq = 0;
uint8_t trampCurBand = 0;
uint8_t trampCurChan = 0;
uint16_t trampCurPower = 0;       // Actual transmitting power
uint16_t trampCurConfigPower = 0; // Configured transmitting power
int16_t trampCurTemp = 0;
uint8_t trampCurPitmode = 0;

uint32_t trampConfFreq = 0;
uint16_t trampConfPower = 0;

#ifdef CMS
static void trampCmsUpdateStatusString(void); // Forward
static void trampCmsUpdateBandChan(void); // Forward
#endif

static void trampWriteBuf(uint8_t *buf)
{
    serialWriteBuf(trampSerialPort, buf, 16);
}

static uint8_t trampChecksum(uint8_t *trampBuf)
{
    uint8_t cksum = 0;

    for (int i = 1 ; i < 14 ; i++)
        cksum += trampBuf[i];

    return cksum;
}

void trampCmdU16(uint8_t cmd, uint16_t param)
{
    if (!trampSerialPort)
        return;

    memset(trampReqBuffer, 0, ARRAYLEN(trampReqBuffer));
    trampReqBuffer[0] = 15;
    trampReqBuffer[1] = cmd;
    trampReqBuffer[2] = param & 0xff;
    trampReqBuffer[3] = (param >> 8) & 0xff;
    trampReqBuffer[14] = trampChecksum(trampReqBuffer);
    trampWriteBuf(trampReqBuffer);
}

void trampSetFreq(uint16_t freq)
{
    trampConfFreq = freq;
}

void trampSendFreq(uint16_t freq)
{
    trampCmdU16('F', freq);
}

void trampSetBandChan(uint8_t band, uint8_t chan)
{
    trampSetFreq(vtx58FreqTable[band - 1][chan - 1]);
}

void trampSetRFPower(uint16_t level)
{
    trampConfPower = level;
}

void trampSendRFPower(uint16_t level)
{
    trampCmdU16('P', level);
}

// return false if error
bool trampCommitChanges()
{
    if(trampStatus != TRAMP_STATUS_ONLINE)
        return false;

    trampStatus = TRAMP_STATUS_SET_FREQ_PW;
    return true;
}

void trampSetPitmode(uint8_t onoff)
{
    trampCmdU16('I', onoff ? 0 : 1);
}

static uint8_t trampPendingQuery = 0; // XXX Assume no code/resp == 0

void trampQuery(uint8_t cmd)
{
    trampPendingQuery = cmd;
    trampCmdU16(cmd, 0);
}

void trampQueryR(void)
{
    trampQuery('r');
}

void trampQueryV(void)
{
    trampQuery('v');
}

void trampQueryS(void)
{
    trampQuery('s');
}

#define TRAMP_SERIAL_OPTIONS (SERIAL_BIDIR)
//#define TRAMP_SERIAL_OPTIONS (0) // For debugging with tramp emulator

bool trampInit()
{
    serialPortConfig_t *portConfig = findSerialPortConfig(FUNCTION_VTX_CONTROL);

    if (portConfig) {
        trampSerialPort = openSerialPort(portConfig->identifier, FUNCTION_VTX_CONTROL, NULL, 9600, MODE_RXTX, TRAMP_SERIAL_OPTIONS);
    }

    if (!trampSerialPort) {
        return false;
    }

    return true;
}

void trampHandleResponse(void)
{
    uint8_t respCode = trampRespBuffer[1];

    switch (respCode) {
    case 'r':
        trampRFFreqMin = trampRespBuffer[2]|(trampRespBuffer[3] << 8);
        trampRFFreqMax = trampRespBuffer[4]|(trampRespBuffer[5] << 8);
        trampRFPowerMax = trampRespBuffer[6]|(trampRespBuffer[7] << 8);
        trampStatus = TRAMP_STATUS_ONLINE;
        break;

    case 'v':
        {
            uint32_t oldCurFreq = trampCurFreq;
            trampCurFreq = trampRespBuffer[2]|(trampRespBuffer[3] << 8);
            trampCurConfigPower = trampRespBuffer[4]|(trampRespBuffer[5] << 8);
            trampCurPower = trampRespBuffer[8]|(trampRespBuffer[9] << 8);
            vtx58_Freq2Bandchan(trampCurFreq, &trampCurBand, &trampCurChan);
            trampCurPitmode = trampRespBuffer[7];

#ifdef CMS
            if(!oldCurFreq && trampCurFreq)
                trampCmsUpdateBandChan();
#endif
            
            if (trampStatus == TRAMP_STATUS_CHECK_FREQ_PW)
                trampStatus = TRAMP_STATUS_SET_FREQ_PW;
        }
        break;

    case 's':
        trampCurTemp = (int16_t)(trampRespBuffer[6]|(trampRespBuffer[7] << 8));
        break;
    }

    if (trampPendingQuery == respCode)
        trampPendingQuery = 0;
}

static bool trampIsValidResponseCode(uint8_t code)
{
    if (code == 'r' || code == 'v' || code == 's')
        return true;
    else
        return false;
}

typedef enum {
    S_WAIT_LEN = 0,   // Waiting for a packet len
    S_WAIT_CODE,      // Waiting for a response code
    S_DATA,           // Waiting for rest of the packet.
} trampReceiveState_e;

static trampReceiveState_e trampReceiveState = S_WAIT_LEN;
static int trampReceivePos = 0;

void trampReceive(uint32_t currentTimeUs)
{
    UNUSED(currentTimeUs);

    if (!trampSerialPort)
        return;

    while (serialRxBytesWaiting(trampSerialPort)) {
        uint8_t c = serialRead(trampSerialPort);
        trampRespBuffer[trampReceivePos++] = c;

        switch(trampReceiveState) {
        case S_WAIT_LEN:
            if (c == 0x0F) {
                trampReceiveState = S_WAIT_CODE;
            } else {
                trampReceivePos = 0;
            }
            break;

        case S_WAIT_CODE:
            if (trampIsValidResponseCode(c)) {
                trampReceiveState = S_DATA;
            } else {
                trampReceiveState = S_WAIT_LEN;
                trampReceivePos = 0;
            }
            break;

        case S_DATA:
            if (trampReceivePos == 16) {
                uint8_t cksum = trampChecksum(trampRespBuffer);
                if ((trampRespBuffer[14] == cksum) && (trampRespBuffer[15] == 0)) {
                    trampHandleResponse();
                }

                trampReceiveState = S_WAIT_LEN;
                trampReceivePos = 0;
            }
            break;

        default:
            trampReceiveState = S_WAIT_LEN;
            trampReceivePos = 0;
        }

        // Debugging...
        if (trampReceivePos >= 16) {
            debug[0]++;
        }
    }
}

void trampProcess(uint32_t currentTimeUs)
{
    static uint32_t lastQueryRTimeUs = 0;
    static uint32_t lastQueryVTimeUs = 0;

    if (trampStatus == TRAMP_STATUS_BAD_DEVICE)
        return;

    trampReceive(currentTimeUs);

    if (trampStatus == TRAMP_STATUS_OFFLINE) {
        if (cmp32(currentTimeUs, lastQueryRTimeUs) > 1000 * 1000) { // 1s
            trampQueryR();
            lastQueryRTimeUs = currentTimeUs;
        }
    } else {
        if (trampStatus == TRAMP_STATUS_SET_FREQ_PW) {

            bool done = false;
            if (trampConfFreq != trampCurFreq) {
                trampSendFreq(trampConfFreq);
            }
            else if (trampConfPower != trampCurPower) {
                trampSendRFPower(trampConfPower);
            }

            if(!done) {
                trampStatus = TRAMP_STATUS_CHECK_FREQ_PW;

                // delay next status query by 200ms
                lastQueryVTimeUs = currentTimeUs + 200 * 1000;
            }
            else {
                // everything has been done, let's return to original state
                trampStatus = TRAMP_STATUS_ONLINE;
            }
        }
        else if (trampStatus == TRAMP_STATUS_CHECK_FREQ_PW) {
            if (cmp32(currentTimeUs, lastQueryVTimeUs) > 50 * 1000) {
                trampQueryV();
                lastQueryVTimeUs = currentTimeUs;
            }
        }
        else if (trampStatus == TRAMP_STATUS_ONLINE) {
            
            if (cmp32(currentTimeUs, lastQueryVTimeUs) > 1000 * 1000) {
                trampQueryV();
                lastQueryVTimeUs = currentTimeUs;
            }
        }

        /* if (trampReceiveState == S_WAIT_LEN) { */
        /*     if (trampPendingQuery) */
        /*         trampQuery(trampPendingQuery); */
        /*     else */
        /*         trampQueryV(); */
        /* } */
    }

#ifdef CMS
    trampCmsUpdateStatusString();
#endif
}

#ifdef CMS
#include "cms/cms.h"
#include "cms/cms_types.h"


char trampCmsStatusString[31] = "- -- ---- ----";
//                               m bc ffff tppp
//                               01234567890123

static void trampCmsUpdateStatusString(void)
{
    trampCmsStatusString[0] = '*';
    trampCmsStatusString[1] = ' ';
    trampCmsStatusString[2] = vtx58BandLetter[trampCurBand];
    trampCmsStatusString[3] = vtx58ChanNames[trampCurChan][0];
    trampCmsStatusString[4] = ' ';

    if (trampCurFreq)
        tfp_sprintf(&trampCmsStatusString[5], "%4d", trampCurFreq);
    else
        tfp_sprintf(&trampCmsStatusString[5], "----");

    if (trampCurPower) {
        tfp_sprintf(&trampCmsStatusString[9], " %c%3d", (trampCurPower == trampCurConfigPower) ? ' ' : '*', trampCurPower);
    }
    else
        tfp_sprintf(&trampCmsStatusString[9], " ----");
}

uint8_t trampCmsPitmode = 0;
uint8_t trampCmsBand = 1;
uint8_t trampCmsChan = 1;
uint16_t trampCmsFreqRef;

static OSD_TAB_t trampCmsEntBand = { &trampCmsBand, 5, vtx58BandNames, NULL };

static OSD_TAB_t trampCmsEntChan = { &trampCmsChan, 8, vtx58ChanNames, NULL };

static OSD_UINT16_t trampCmsEntFreqRef = { &trampCmsFreqRef, 5600, 5900, 0 };

static void trampCmsUpdateFreqRef(void)
{
    if (trampCmsBand > 0 && trampCmsChan > 0)
        trampCmsFreqRef = vtx58FreqTable[trampCmsBand - 1][trampCmsChan - 1];
}

static void trampCmsUpdateBandChan()
{
    if(trampCurBand > 0) trampCmsBand = trampCurBand;
    if(trampCurChan > 0) trampCmsChan = trampCurChan;
    trampCmsUpdateFreqRef();
    trampCmsPitmode = trampCurPitmode + 1;
}

static long trampCmsConfigBand(displayPort_t *pDisp, const void *self)
{
    UNUSED(pDisp);
    UNUSED(self);

    if (trampCmsBand == 0)
        // Bounce back
        trampCmsBand = 1;
    else
        trampCmsUpdateFreqRef();

    return 0;
}

static long trampCmsConfigChan(displayPort_t *pDisp, const void *self)
{
    UNUSED(pDisp);
    UNUSED(self);

    if (trampCmsChan == 0)
        // Bounce back
        trampCmsChan = 1;
    else
        trampCmsUpdateFreqRef();

    return 0;
}

static const char * const trampCmsPowerNames[] = {
    "25 ", "100", "200", "400", "600"
};

static const uint16_t trampCmsPowerTable[] = {
    25, 100, 200, 400, 600
};

static uint8_t trampCmsPower = 0;

static OSD_TAB_t trampCmsEntPower = { &trampCmsPower, 4, trampCmsPowerNames, NULL };

static OSD_INT16_t trampCmsEntTemp = { &trampCurTemp, -100, 300, 0 };

static const char * const trampCmsPitmodeNames[] = {
    "---", "OFF", "ON "
};

static OSD_TAB_t trampCmsEntPitmode = { &trampCmsPitmode, 2, trampCmsPitmodeNames, NULL };

static long trampCmsSetPitmode(displayPort_t *pDisp, const void *self)
{
    UNUSED(pDisp);
    UNUSED(self);

    if (trampCmsPitmode == 0) {
        // Bouce back
        trampCmsPitmode = 1;
    } else {
        trampSetPitmode(trampCmsPitmode - 1);
    }

    return 0;
}

static long trampCmsCommence(displayPort_t *pDisp, const void *self)
{
    UNUSED(pDisp);
    UNUSED(self);

    trampSetBandChan(trampCmsBand, trampCmsChan);
    trampSetRFPower(trampCmsPowerTable[trampCmsPower]);

    // TODO: error handling
    trampCommitChanges();

    trampCmsFreqRef = vtx58FreqTable[trampCmsBand - 1][trampCmsChan - 1];

    return MENU_CHAIN_BACK;
}

static OSD_Entry trampCmsMenuCommenceEntries[] = {
    { "CONFIRM", OME_Label,   NULL,          NULL, 0 },
    { "YES",     OME_Funcall, trampCmsCommence, NULL, 0 },
    { "BACK",    OME_Back, NULL, NULL, 0 },
    { NULL,      OME_END, NULL, NULL, 0 }
};

static CMS_Menu trampCmsMenuCommence = {
    .GUARD_text = "XVTXTRC",
    .GUARD_type = OME_MENU,
    .onEnter = NULL,
    .onExit = NULL,
    .onGlobalExit = NULL,
    .entries = trampCmsMenuCommenceEntries,
};

static OSD_Entry trampMenuEntries[] =
{
    { "- TRAMP -", OME_Label, NULL, NULL, 0 },

    { "",       OME_Label,   NULL,                   trampCmsStatusString,  DYNAMIC },
    { "PIT",    OME_TAB,     trampCmsSetPitmode,     &trampCmsEntPitmode,   0 },
    { "BAND",   OME_TAB,     trampCmsConfigBand,     &trampCmsEntBand,      0 },
    { "CHAN",   OME_TAB,     trampCmsConfigChan,     &trampCmsEntChan,      0 },
    { "(FREQ)", OME_UINT16,  NULL,                   &trampCmsEntFreqRef,   DYNAMIC },
    { "POWER",  OME_TAB,     NULL,                   &trampCmsEntPower,     0 },
    { "TEMP",   OME_INT16,   NULL,                   &trampCmsEntTemp,      DYNAMIC },
    { "SET",    OME_Submenu, cmsMenuChange,          &trampCmsMenuCommence, 0 },
    //{ "CONFIG", OME_Submenu, cmsMenuChange,          &saCmsMenuConfig,   0 },

    { "BACK",   OME_Back, NULL, NULL, 0 },
    { NULL,     OME_END, NULL, NULL, 0 }
};

CMS_Menu cmsx_menuVtxTramp = {
    .GUARD_text = "XVTXTR",
    .GUARD_type = OME_MENU,
    .onEnter = NULL,
    .onExit = NULL,
    .onGlobalExit = NULL,
    .entries = trampMenuEntries,
};
#endif

#endif // VTX_TRAMP
