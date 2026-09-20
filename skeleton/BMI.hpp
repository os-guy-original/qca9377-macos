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

// bmi.h:59-79 — command IDs.
static const uint32_t kBmiNoCommand     = 0;
static const uint32_t kBmiDone          = 1;
static const uint32_t kBmiReadMemory    = 2;
static const uint32_t kBmiWriteMemory   = 3;
static const uint32_t kBmiExecute       = 4;
static const uint32_t kBmiGetTargetInfo = 8;   // BMI_GET_TARGET_INFO
static const uint32_t kBmiLzStreamStart = 13;  // followed by LZ_DATA
static const uint32_t kBmiLzData        = 14;

// bmi.h:40 — per-command data cap (drives the LZ streaming chunk size).
static const uint32_t kBmiMaxDataSize   = 256;

// Wire format (bmi.h:99-129). Empty payload structs are intentional:
// e.g. GET_TARGET_INFO carries the 4-byte id ONLY (bmi.h:128-129).
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
    uint32_t len;      // requested response length (<= kBmiMaxDataSize)
} __attribute__((packed));
// WRITE_MEMORY / LZ_DATA have variable payloads; built in the cpp.

// Responses (bmi.h:161-175): payload-only — no response id field.
struct BmiRespGetTargetInfo {
    uint32_t len;      // = 8
    uint32_t version;
    uint32_t type;
} __attribute__((packed));
struct BmiRespReadMemory  { uint8_t payload[kBmiMaxDataSize]; } __attribute__((packed));
struct BmiRespExecute     { uint32_t result; } __attribute__((packed));

static_assert(sizeof(BmiCmdGetTargetInfo) == 4, "bmi cmd size");
static_assert(sizeof(BmiRespGetTargetInfo) == 12, "bmi resp size");

class Bmi {
public:
    explicit Bmi(CopyEngine *ce) : fCe(ce) {}

    // ---- M2 ----
    // Send BMI_GET_TARGET_INFO over CE0, poll CE1 for the response.
    // On success fills fTargetVersion/fTargetType and returns true.
    bool getTargetInfo();

    uint32_t targetVersion() const { return fTargetVersion; }
    uint32_t targetType()    const { return fTargetType; }

    // Decode helper: Linux logs "target 0x05020001" for this card
    // (hw ground truth). We log raw + a family decode.
    static void logTargetVersion(uint32_t version);

    // ---- M3 ----
    // BMI_READ_MEMORY: target RAM/registers -> host (bmi.c:155-196).
    bool readMemory(uint32_t addr, void *out, uint32_t len);

    // BMI_WRITE_MEMORY: host -> target RAM (bmi.c:260-313). Chunks at
    // 4-byte alignment, mirroring the roundup() in ath10k.
    bool writeMemory(uint32_t addr, const void *buf, uint32_t len);

    // ath10k_bmi_read32/write32 (bmi.h:242-269): HI-item accessor =
    // 32-bit memory op at HOST_INTEREST_ADDRESS + item offset.
    bool read32(uint32_t itemOffset, uint32_t *val);
    bool write32(uint32_t itemOffset, uint32_t val);

    // BMI_EXECUTE: run code at addr, block for the u32 result
    // (bmi.c:316-347). Used for the OTP board-id query.
    bool execute(uint32_t addr, uint32_t param, uint32_t *result);

    // LZ fast-download (bmi.c:432-495): LZ_STREAM_START(addr),
    // LZ_DATA chunks (zero-padded trailer to 4B), LZ_STREAM_START(0)
    // to flush target caches.
    bool fastDownload(uint32_t addr, const void *buf, uint32_t len);

    // BMI_DONE (bmi.c:103-127): tells the ROM BMI is finished. After this
    // no further BMI commands are legal.
    bool done();

private:
    CopyEngine *fCe = nullptr;
    uint32_t    fTargetVersion = 0;
    uint32_t    fTargetType    = 0;
    bool        fDoneSent      = false;

    // 12-byte buffer for the three read-style responses.
    bool exchangeWait(const void *cmd, uint32_t cmdLen,
                      void *resp, uint32_t respLen, uint32_t *gotLen);
};

} // namespace qca

#endif /* QCA9377_BMI_hpp */
