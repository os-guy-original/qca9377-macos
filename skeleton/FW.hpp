/*
 * FW.hpp — firmware container parsing + BMI boot sequence for QCA9377 (M3).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k (drivers/net/wireless/ath/ath10k/
 * core.c, core.h, hw.h, targaddrs.h), ISC-licensed: Copyright (c) Atheros
 * Communications Inc., Copyright (c) Qualcomm Atheros, Inc. Adapted per
 * ISC terms with attribution. Derived from Linux v7.2.3 ath10k,
 * sha256-pinned: 8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03.
 *
 * Container format (core.c:2009-2187, verified empirically against the
 * pinned binaries — see LOG session 11):
 *   "QCA-ATH10K\0" magic, padded to 4; then IEs:
 *     { le32 id, le32 len, payload, pad-to-4 }
 *   ids: 0=FW_VERSION 1=TIMESTAMP 2=FEATURES 3=FW_IMAGE 4=OTP_IMAGE
 *        5=WMI_OP_VER 6=HTT_OP_VER 7=CODE_SWAP (core.h IE enum, hw.h:149-182)
 *
 * board-2.bin: magic "QCA-ATH10K-BOARD\0" then one IE per vendor
 * (ATH10K_BD_IE_BOARD=0, hw.h:217); inside each, sub-IEs
 * BOARD_NAME=0 / BOARD_DATA=1 (hw.h:221-224). Name matched against
 * "bus=pci,vendor=...,device=...,subsystem-vendor=...,subsystem-device=..."
 * (core.c: ath10k_core_create_board_name).
 */

#ifndef QCA9377_FW_hpp
#define QCA9377_FW_hpp

#include "BMI.hpp"
#include <stdint.h>

namespace qca {

enum FwIe : uint32_t {
    kIeFwVersion   = 0,
    kIeTimestamp   = 1,
    kIeFeatures    = 2,
    kIeFwImage     = 3,
    kIeOtpImage    = 4,
    kIeWmiOpVer    = 5,
    kIeHttOpVer    = 6,
    kIeCodeSwap    = 7,
};

enum BoardIe : uint32_t {
    kBdIeBoardName = 0,
    kBdIeBoardData = 1,
};

struct FwImage {
    const uint8_t *firmware = nullptr;  uint32_t firmwareLen = 0;
    const uint8_t *otp      = nullptr;  uint32_t otpLen      = 0;
    char           version[64] = {0};
};

class Fw {
public:

    static bool parseFirmware(FwImage *out);

    static bool selectBoard(const uint8_t *board2, uint32_t board2Len,
                            uint16_t subsystemVendor, uint16_t subsystemDevice,
                            const uint8_t **outData, uint32_t *outLen);

    static bool configureTarget(Bmi *bmi);

    static bool downloadBoardData(Bmi *bmi,
                                  const uint8_t *boardData, uint32_t boardLen);

    static bool runOtp(Bmi *bmi, const uint8_t *otp, uint32_t otpLen,
                       uint32_t *boardId, uint32_t *chipId);

    static bool downloadFirmware(Bmi *bmi,
                                 const uint8_t *fw, uint32_t fwLen);
    static bool doneAndWaitTargetInit(Bmi *bmi, class CopyEngine *unusedCe,
                                      volatile uint32_t *bar0);

    static const uint32_t kPatchLoadAddr      = 0x1234;
    static const uint32_t kHiBoardDataOff     = 0x54;
    static const uint32_t kHiBoardDataInitOff = 0x58;
    static const uint32_t kFwIndicatorAddr    = 0x3a028;
    static const uint32_t kFwIndEventPending  = 1;
    static const uint32_t kFwIndInitialized   = 2;
};

}

#endif
