/*
 * BMI.hpp — Boot Module Interface protocol for QCA9377 (M2+M3).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k (drivers/net/wireless/ath/ath10k/bmi.h,
 * bmi.c, pci.c), ISC-licensed: Copyright (c) Atheros Communications Inc.,
 * Copyright (c) Qualcomm Atheros, Inc. Adapted per ISC terms with
 * attribution. Derived from Linux v7.2.3 ath10k, sha256-pinned:
 * 8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03.
 *
 * BMI is the target's ROM boot monitor: the host sends commands over CE0
 * and reads responses over CE1 (bmi.h:228-229).
 *
 * M2: BMI_GET_TARGET_INFO (ROM alive proof).
 * M3: read/write memory (HI items), execute (OTP), LZ streaming (firmware),
 *     BMI_DONE (BMI shutdown). Send-only commands wait for send completion
 *     only — no recv is posted (pci.c:2110-2112, 2176).
 */

#ifndef QCA9377_BMI_hpp
#define QCA9377_BMI_hpp

#include "CE.hpp"
#include <stdint.h>

namespace qca {

static const uint32_t kBmiNoCommand     = 0;
static const uint32_t kBmiDone          = 1;
static const uint32_t kBmiReadMemory    = 2;
static const uint32_t kBmiWriteMemory   = 3;
static const uint32_t kBmiExecute       = 4;
static const uint32_t kBmiGetTargetInfo = 8;
static const uint32_t kBmiLzStreamStart = 13;
static const uint32_t kBmiLzData        = 14;

static const uint32_t kBmiMaxDataSize   = 256;

struct BmiCmdGetTargetInfo { uint32_t id; } __attribute__((packed));
struct BmiCmdDone          { uint32_t id; } __attribute__((packed));
struct BmiCmdLzStreamStart { uint32_t id; uint32_t addr; } __attribute__((packed));
struct BmiCmdExecute {
    uint32_t id;
    uint32_t addr;
    uint32_t param;
} __attribute__((packed));
struct BmiCmdReadMemory {
    uint32_t id;
    uint32_t addr;
    uint32_t len;
} __attribute__((packed));

struct BmiRespGetTargetInfo {
    uint32_t len;
    uint32_t version;
    uint32_t type;
} __attribute__((packed));
struct BmiRespReadMemory  { uint8_t payload[kBmiMaxDataSize]; } __attribute__((packed));
struct BmiRespExecute     { uint32_t result; } __attribute__((packed));

static_assert(sizeof(BmiCmdGetTargetInfo) == 4, "bmi cmd size");
static_assert(sizeof(BmiRespGetTargetInfo) == 12, "bmi resp size");

class Bmi {
public:
    explicit Bmi(CEManager *ce) : fCe(ce) {}

    bool getTargetInfo();

    uint32_t targetVersion() const { return fTargetVersion; }
    uint32_t targetType()    const { return fTargetType; }

    static void logTargetVersion(uint32_t version);

    bool readMemory(uint32_t addr, void *out, uint32_t len);

    bool writeMemory(uint32_t addr, const void *buf, uint32_t len);

    bool read32(uint32_t itemOffset, uint32_t *val);
    bool write32(uint32_t itemOffset, uint32_t val);

    bool execute(uint32_t addr, uint32_t param, uint32_t *result);

    bool fastDownload(uint32_t addr, const void *buf, uint32_t len);

    bool done();

private:
    CEManager *fCe = nullptr;
    uint32_t    fTargetVersion = 0;
    uint32_t    fTargetType    = 0;
    bool        fDoneSent      = false;

    bool exchangeWait(const void *cmd, uint32_t cmdLen,
                      void *resp, uint32_t respLen, uint32_t *gotLen);
};

}

#endif
