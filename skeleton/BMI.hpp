/*
 * BMI.hpp — Boot Module Interface protocol for QCA9377 (M2).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k (drivers/net/wireless/ath/ath10k/bmi.h,
 * bmi.c, pci.c), ISC-licensed: Copyright (c) Atheros Communications Inc.,
 * Copyright (c) Qualcomm Atheros, Inc. Adapted per ISC terms with
 * attribution. Derived from Linux v7.2.3 ath10k, sha256-pinned:
 * 8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03.
 *
 * BMI is the target's ROM boot monitor: the host sends commands over CE0
 * and reads responses over CE1 (bmi.h:228-229). M2 goal: one command —
 * BMI_GET_TARGET_INFO — answered by the ROM proves the whole DMA path.
 * No firmware files are involved at this stage.
 */

#ifndef QCA9377_BMI_hpp
#define QCA9377_BMI_hpp

#include "CE.hpp"
#include <stdint.h>

namespace qca {

// bmi.h:60-61+ — command IDs (only what M2 needs).
static const uint32_t kBmiNoCommand     = 0;
static const uint32_t kBmiDone          = 1;
static const uint32_t kBmiGetTargetInfo = 8;   // BMI_GET_TARGET_INFO

// Wire format (bmi.c:48-78): command = u32 id [+ per-cmd payload];
// get_target_info response = u32 version.
struct BmiCmdGetTargetInfo {
    uint32_t id;      // LE on the wire
    uint32_t unused;  // kept explicit: 8-byte exchange
} __attribute__((packed));

static_assert(sizeof(BmiCmdGetTargetInfo) == 8, "bmi cmd size");

class Bmi {
public:
    explicit Bmi(CopyEngine *ce) : fCe(ce) {}

    // Send BMI_GET_TARGET_INFO over CE0, poll CE1 for the response.
    // On success fills fTargetVersion and returns true.
    bool getTargetInfo();

    uint32_t targetVersion() const { return fTargetVersion; }

    // Decode helper: Linux logs "target 0x05020001" for this card
    // (hw ground truth). We log raw + a family decode.
    static void logTargetVersion(uint32_t version);

private:
    CopyEngine *fCe = nullptr;
    uint32_t    fTargetVersion = 0;
};

} // namespace qca

#endif /* QCA9377_BMI_hpp */
