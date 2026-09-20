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

// ------------------------------------------------------------------
// LE readers for the container fields
// ------------------------------------------------------------------
static inline uint32_t rdLe32(const uint8_t *p)
{
    return OSReadLittleInt32(p, 0);
}

// ------------------------------------------------------------------
// firmware-6.bin parser — port of ath10k_core_fetch_firmware_api_n
// (core.c:2009-2187), validated empirically against the pinned file
// (session 11: FW_VERSION len=28, OTP 24193B, FW_IMAGE 727125B).
// ------------------------------------------------------------------
bool Fw::parseFirmware(FwImage *out)
{
    const uint8_t *data = qca9377_firmware6_bin;
    uint32_t len = qca9377_firmware6_len;

    static const char kMagic[] = "QCA-ATH10K";     // core.h:149
    const uint32_t magicLen = sizeof(kMagic);      // includes the NUL

    if (len < magicLen || bcmp(data, kMagic, magicLen) != 0) {
        IOLog("QCA9377-FW: bad firmware magic\n");
        return false;
    }
    const uint32_t magicPad = (magicLen + 3) & ~3u;
    data += magicPad;
    len  -= magicPad;

    while (len > 8) {                              // sizeof(ath10k_fw_ie)
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
            break;   // TIMESTAMP/FEATURES/WMI/HTT/CODE_SWAP: not needed yet
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

// ------------------------------------------------------------------
// board-2.bin selection — port of ath10k_core_parse_bd_ie_board
// (core.c:1509-1573). Layout (verified empirically, 37 vendor entries):
//   outer IE 0 (BOARD) wraps repeated: NAME sub-IE then DATA sub-IE.
// ------------------------------------------------------------------
bool Fw::selectBoard(const uint8_t *board2, uint32_t board2Len,
                     uint16_t subsystemVendor, uint16_t subsystemDevice,
                     const uint8_t **outData, uint32_t *outLen)
{
    static const char kMagic[] = "QCA-ATH10K-BOARD";  // core.h:150
    const uint32_t magicLen = sizeof(kMagic);

    if (board2Len < magicLen || bcmp(board2, kMagic, magicLen) != 0) {
        IOLog("QCA9377-FW: bad board magic\n");
        return false;
    }
    uint32_t magicPad = (magicLen + 3) & ~3u;
    const uint8_t *data = board2 + magicPad;
    uint32_t len = board2Len - magicPad;

    // The name ath10k would build for this machine
    // (core.c: ath10k_core_create_board_name, PCI no-bmi-ids branch).
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

// ------------------------------------------------------------------
// configureTarget (core.c:867-930)
// ------------------------------------------------------------------
bool Fw::configureTarget(Bmi *bmi)
{
    // HI item offsets from targaddrs.h struct host_interest:
    //   hi_app_host_interest 0x00, hi_option_flag 0x10,
    //   hi_be 0x44, hi_fw_swap 0x104.
    // HTC_PROTOCOL_VERSION = 2 (wmi.h:64).
    if (!bmi->write32(0x00, 2)) {
        IOLog("QCA9377-FW: htc version write failed\n");
        return false;
    }

    // hi_option_flag: read-modify-write (core.c:878-904).
    uint32_t opt = 0;
    if (!bmi->read32(0x10, &opt)) {
        IOLog("QCA9377-FW: option_flag read failed\n");
        return false;
    }
    // HI_OPTION_NUM_DEV_SHIFT 0x9, value 1 (targaddrs.h:272);
    // FW_MODE_AP 0x2 << 0xC (279, 287); MAC_ADDR_METHOD_SHIFT 3 (256);
    // FW_BRIDGE 0 << 4; FW_SUBMODE 0 << 0x14.
    opt |= (1u << 9) | (0x2u << 0xC) | (1u << 3);
    if (!bmi->write32(0x10, opt)) {
        IOLog("QCA9377-FW: option_flag write failed\n");
        return false;
    }

    // "We do all byte-swapping on the host" — hi_be=0, hi_fw_swap=0.
    if (!bmi->write32(0x44, 0) || !bmi->write32(0x104, 0)) {
        IOLog("QCA9377-FW: be/fw_swap write failed\n");
        return false;
    }
    IOLog("QCA9377-FW: target configured (htc=2, opt=0x%08x)\n", opt);
    return true;
}

// ------------------------------------------------------------------
// downloadBoardData (core.c:1742-1790; ext-data skipped: ext size 0)
// ------------------------------------------------------------------
bool Fw::downloadBoardData(Bmi *bmi,
                           const uint8_t *boardData, uint32_t boardLen)
{
    // hi_board_data points into target RAM; ath10k reads it rather than
    // hardcoding. If the read returns 0 the target has no board area.
    uint32_t boardAddr = 0;
    if (!bmi->read32(kHiBoardDataOff, &boardAddr)) {
        IOLog("QCA9377-FW: hi_board_data read failed\n");
        return false;
    }
    if (boardAddr == 0) {
        IOLog("QCA9377-FW: hi_board_data == 0 (no board area)\n");
        return false;
    }

    // Cap at QCA9377_BOARD_DATA_SZ = QCA6174_BOARD_DATA_SZ = 8192
    // (targaddrs.h:478-482); board-2 entries are exactly 8124B of data.
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

// ------------------------------------------------------------------
// runOtp (core.c:1016-1080 download, 1811+ run; decode bmi.h:91-97)
// ------------------------------------------------------------------
bool Fw::runOtp(Bmi *bmi, const uint8_t *otp, uint32_t otpLen,
                uint32_t *boardId, uint32_t *chipId)
{
    if (!otp || !otpLen)
        return false;

    if (!bmi->fastDownload(kPatchLoadAddr, otp, otpLen)) {
        IOLog("QCA9377-FW: otp download failed\n");
        return false;
    }

    // BMI_PARAM_GET_EEPROM_BOARD_ID = 0x10 (bmi.h:83); flash/DT cal paths
    // do not apply on this machine (no cal file, no DT node).
    uint32_t result = 0;
    if (!bmi->execute(kPatchLoadAddr, 0x10, &result)) {
        IOLog("QCA9377-FW: otp execute failed\n");
        return false;
    }

    // ATH10K_BMI_BOARD_ID_FROM_OTP mask 0x7c00 lsb 10;
    // CHIP_ID mask 0x18000 lsb 15; status mask 0xff (bmi.h:91-97).
    *boardId = (result & 0x7c00u) >> 10;
    *chipId  = (result & 0x18000u) >> 15;
    const bool ok = ((result & 0xffu) == 0) && (*boardId != 0);
    IOLog("QCA9377-FW: otp result 0x%08x board=%u chip=%u valid=%s\n",
          result, *boardId, *chipId, ok ? "yes" : "no");
    return true;   // board-id-0 is legitimate: caller falls back to board.bin
}

// ------------------------------------------------------------------
// downloadFirmware (core.c:1184-1234 -> bmi.c fast_download)
// ------------------------------------------------------------------
bool Fw::downloadFirmware(Bmi *bmi, const uint8_t *fw, uint32_t fwLen)
{
    if (!bmi->fastDownload(kPatchLoadAddr, fw, fwLen)) {
        IOLog("QCA9377-FW: firmware download failed\n");
        return false;
    }
    IOLog("QCA9377-FW: firmware %uB -> 0x%08x OK\n", fwLen, kPatchLoadAddr);
    return true;
}

// ------------------------------------------------------------------
// BMI_DONE + target-init wait (bmi.c:103-127, pci.c:3284-3345)
// ------------------------------------------------------------------
bool Fw::doneAndWaitTargetInit(Bmi *bmi, class CopyEngine *unusedCe,
                               volatile uint32_t *bar0)
{
    (void)unusedCe;
    if (!bmi->done())
        return false;

    // ath10k reads FW_INDICATOR_ADDRESS flat through BAR0 (no diag window
    // on this path); 3s timeout (ATH10K_PCI_TARGET_WAIT, pci.c:42).
    const uint32_t deadline = 3000 * 1000 / kQCAPollStep_us;
    uint32_t val = 0;
    for (uint32_t i = 0; i < deadline; i++) {
        val = OSReadLittleInt32(bar0, kFwIndicatorAddr);
        if (val == 0xffffffffu)
            continue;                       // device gone / not ready
        if (val & kFwIndEventPending)
            break;                          // crashed during init
        if (val & kFwIndInitialized)
            break;                          // success
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

} // namespace qca
