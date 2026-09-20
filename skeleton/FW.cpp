/*
 * FW.cpp — firmware container parsing + BMI boot sequence (M3).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k core.c/targaddrs.h (see FW.hpp for
 * attribution and pin).
 */

#include "FW.hpp"
#include "FwData.h"
#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

namespace qca {

static inline uint32_t rdLe32(const uint8_t *p)
{
    return OSReadLittleInt32(p, 0);
}

// (core.c:2009-2187), validated empirically against the pinned file

bool Fw::parseFirmware(FwImage *out)
{
    const uint8_t *data = qca9377_firmware6_bin;
    uint32_t len = qca9377_firmware6_len;

    static const char kMagic[] = "QCA-ATH10K";
    const uint32_t magicLen = sizeof(kMagic);

    if (len < magicLen || bcmp(data, kMagic, magicLen) != 0) {
        IOLog("QCA9377-FW: bad firmware magic\n");
        return false;
    }
    const uint32_t magicPad = (magicLen + 3) & ~3u;
    data += magicPad;
    len  -= magicPad;

    while (len > 8) {
        const uint32_t ieId  = rdLe32(data);
        const uint32_t ieLen = rdLe32(data + 4);
        data += 8;
        len  -= 8;
        if (len < ieLen) {
            IOLog("QCA9377-FW: truncated IE %u (len %u > %u left)\n",
                  ieId, ieLen, len);
            return false;
        }

        switch (ieId) {
        case kIeFwVersion:
            if (ieLen < sizeof(out->version)) {
                bcopy(data, out->version, ieLen);
                out->version[ieLen] = '\0';
            }
            break;
        case kIeFwImage:
            out->firmware    = data;
            out->firmwareLen = ieLen;
            break;
        case kIeOtpImage:
            out->otp    = data;
            out->otpLen = ieLen;
            break;
        default:
            break;
        }

        const uint32_t pad = (ieLen + 3) & ~3u;
        data += pad;
        len  -= pad;
    }

    if (!out->firmware || !out->firmwareLen) {
        IOLog("QCA9377-FW: no FW_IMAGE IE found\n");
        return false;
    }
    IOLog("QCA9377-FW: fw6 parsed: version='%s' fw=%uB otp=%uB\n",
          out->version, out->firmwareLen, out->otpLen);
    return true;
}

bool Fw::selectBoard(const uint8_t *board2, uint32_t board2Len,
                     uint16_t subsystemVendor, uint16_t subsystemDevice,
                     const uint8_t **outData, uint32_t *outLen)
{
    static const char kMagic[] = "QCA-ATH10K-BOARD";
    const uint32_t magicLen = sizeof(kMagic);

    if (board2Len < magicLen || bcmp(board2, kMagic, magicLen) != 0) {
        IOLog("QCA9377-FW: bad board magic\n");
        return false;
    }
    uint32_t magicPad = (magicLen + 3) & ~3u;
    const uint8_t *data = board2 + magicPad;
    uint32_t len = board2Len - magicPad;

    char want[64];
    snprintf(want, sizeof(want),
             "bus=pci,vendor=168c,device=0042,"
             "subsystem-vendor=%04x,subsystem-device=%04x",
             subsystemVendor, subsystemDevice);
    const uint32_t wantLen = (uint32_t)strlen(want);

    bool nameMatch = false;
    while (len > 8) {
        const uint32_t ieId  = rdLe32(data);
        const uint32_t ieLen = rdLe32(data + 4);
        data += 8;
        len  -= 8;
        if (len < ieLen) break;

        if (ieId == kBdIeBoardName) {
            nameMatch = (ieLen == wantLen &&
                         bcmp(data, want, wantLen) == 0);
            if (nameMatch) {
                IOLog("QCA9377-FW: board name match: '%s'\n", want);
            }
        } else if (ieId == kBdIeBoardData && nameMatch) {
            *outData = data;
            *outLen  = ieLen;
            IOLog("QCA9377-FW: board data found (%u bytes)\n", ieLen);
            return true;
        }

        const uint32_t pad = (ieLen + 3) & ~3u;
        data += pad;
        len  -= pad;
    }

    IOLog("QCA9377-FW: no board data for '%s'\n", want);
    return false;
}

bool Fw::configureTarget(Bmi *bmi)
{

    if (!bmi->write32(0x00, 2)) {
        IOLog("QCA9377-FW: htc version write failed\n");
        return false;
    }

    uint32_t opt = 0;
    if (!bmi->read32(0x10, &opt)) {
        IOLog("QCA9377-FW: option_flag read failed\n");
        return false;
    }

    opt |= (1u << 9) | (0x2u << 0xC) | (1u << 3);
    if (!bmi->write32(0x10, opt)) {
        IOLog("QCA9377-FW: option_flag write failed\n");
        return false;
    }

    if (!bmi->write32(0x44, 0) || !bmi->write32(0x104, 0)) {
        IOLog("QCA9377-FW: be/fw_swap write failed\n");
        return false;
    }
    IOLog("QCA9377-FW: target configured (htc=2, opt=0x%08x)\n", opt);
    return true;
}

bool Fw::downloadBoardData(Bmi *bmi,
                           const uint8_t *boardData, uint32_t boardLen)
{

    uint32_t boardAddr = 0;
    if (!bmi->read32(kHiBoardDataOff, &boardAddr)) {
        IOLog("QCA9377-FW: hi_board_data read failed\n");
        return false;
    }
    if (boardAddr == 0) {
        IOLog("QCA9377-FW: hi_board_data == 0 (no board area)\n");
        return false;
    }

    const uint32_t kBoardDataSz = 8192;
    const uint32_t n = boardLen < kBoardDataSz ? boardLen : kBoardDataSz;
    if (!bmi->writeMemory(boardAddr, boardData, n)) {
        IOLog("QCA9377-FW: board write @0x%08x failed\n", boardAddr);
        return false;
    }
    if (!bmi->write32(kHiBoardDataInitOff, 1)) {
        IOLog("QCA9377-FW: board init flag write failed\n");
        return false;
    }
    IOLog("QCA9377-FW: board data %uB -> 0x%08x, initialized=1\n",
          n, boardAddr);
    return true;
}

bool Fw::runOtp(Bmi *bmi, const uint8_t *otp, uint32_t otpLen,
                uint32_t *boardId, uint32_t *chipId)
{
    if (!otp || !otpLen)
        return false;

    if (!bmi->fastDownload(kPatchLoadAddr, otp, otpLen)) {
        IOLog("QCA9377-FW: otp download failed\n");
        return false;
    }

    // do not apply on this machine (no cal file, no DT node).
    uint32_t result = 0;
    if (!bmi->execute(kPatchLoadAddr, 0x10, &result)) {
        IOLog("QCA9377-FW: otp execute failed\n");
        return false;
    }

    *boardId = (result & 0x7c00u) >> 10;
    *chipId  = (result & 0x18000u) >> 15;
    const bool ok = ((result & 0xffu) == 0) && (*boardId != 0);
    IOLog("QCA9377-FW: otp result 0x%08x board=%u chip=%u valid=%s\n",
          result, *boardId, *chipId, ok ? "yes" : "no");
    return true;
}

bool Fw::downloadFirmware(Bmi *bmi, const uint8_t *fw, uint32_t fwLen)
{
    if (!bmi->fastDownload(kPatchLoadAddr, fw, fwLen)) {
        IOLog("QCA9377-FW: firmware download failed\n");
        return false;
    }
    IOLog("QCA9377-FW: firmware %uB -> 0x%08x OK\n", fwLen, kPatchLoadAddr);
    return true;
}

bool Fw::doneAndWaitTargetInit(Bmi *bmi, class CEManager *unusedCe,
                               volatile uint32_t *bar0)
{
    (void)unusedCe;
    if (!bmi->done())
        return false;

    const uint32_t deadline = 3000 * 1000 / kQCAPollStep_us;
    uint32_t val = 0;
    for (uint32_t i = 0; i < deadline; i++) {
        val = OSReadLittleInt32(bar0, kFwIndicatorAddr);
        if (val == 0xffffffffu)
            continue;
        if (val & kFwIndEventPending)
            break;
        if (val & kFwIndInitialized)
            break;
        IODelay(kQCAPollStep_us);
    }

    if (val == 0xffffffffu) {
        IOLog("QCA9377-FW: target init wait: device gone\n");
        return false;
    }
    if (val & kFwIndEventPending) {
        IOLog("QCA9377-FW: target crashed during init (ind=0x%08x)\n", val);
        return false;
    }
    if (!(val & kFwIndInitialized)) {
        IOLog("QCA9377-FW: target init timeout (ind=0x%08x)\n", val);
        return false;
    }
    IOLog("QCA9377-FW: TARGET INITIALIZED (ind=0x%08x)\n", val);
    return true;
}

}
