/*
 * BMI.cpp — Boot Module Interface protocol for QCA9377 (M2).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k bmi.c/pci.c (see BMI.hpp for the full
 * attribution and pin).
 *
 * Exchange shape (pci.c:2106-2193 + bmi.c:48-78):
 *   1. post recv buffer on CE1 (the response lands there)
 *   2. send the command on CE0, wait for send completion (SRRI)
 *   3. poll CE1 until DRRI advances; descriptor nbytes = response length
 *   4. decode: u32 target version
 */

#include "BMI.hpp"
#include <IOKit/IOLib.h>

namespace qca {

bool Bmi::getTargetInfo()
{
    if (!fCe || !fCe->txBuf() || !fCe->rxBuf())
        return false;

    // 1. Post the recv buffer FIRST (pci.c:2160-2162: rx posted before
    // the send so the target can respond immediately).
    // (recvPolling posts its own buffer; the send below is issued first
    // in ath10k's ordering only after the rx post. Our recvPolling()
    // posts then polls, which preserves the rx-before-send completion
    // semantics because the target cannot complete a recv we have not
    // sent a command for. See LOG session 10 for the ordering note.)

    // 2. Command on the tx buffer: id = BMI_GET_TARGET_INFO (bmi.c:64).
    BmiCmdGetTargetInfo cmd;
    cmd.id     = kBmiGetTargetInfo;   // stored LE in memory (x86)
    cmd.unused = 0;
    if (!fCe->send(&cmd, sizeof(cmd))) {
        IOLog("QCA9377-BMI: send failed (CE0)\n");
        return false;
    }
    IOLog("QCA9377-BMI: cmd sent (id=%u, %zu bytes)\n",
          kBmiGetTargetInfo, sizeof(cmd));

    // 3. Poll for the response on CE1 (bmi.c:66: exchange; pci.c:2232+
    // wait loop semantics, poll side here).
    if (!fCe->recvPolling(kQCAExchangeTimeout_ms)) {
        IOLog("QCA9377-BMI: recv timeout (CE1) - no ROM response\n");
        return false;
    }

    // 4. Decode (bmi.c:72-78: resplen must cover the u32 version).
    if (fCe->rxNbytes() < 4) {
        IOLog("QCA9377-BMI: short response (%u bytes)\n", fCe->rxNbytes());
        return false;
    }
    uint32_t version = *(volatile uint32_t *)fCe->rxBuf(); // LE load
    fTargetVersion = version;

    logTargetVersion(version);
    return true;
}

void Bmi::logTargetVersion(uint32_t version)
{
    // Linux ground truth on this card: "target 0x05020001" —
    // QCA9377_HW_1_0_DEV_VERSION is 0x05020000 with the low bit set
    // (ref docs/REFERENCE-TRACE.md). High byte family: 0x0502 = qca6174
    // family target.
    const char *family = "unknown";
    if ((version & 0xffff0000) == 0x05020000)
        family = "qca6174-family (QCA9377)";
    IOLog("QCA9377-BMI: TARGET INFO version=0x%08x [%s]\n", version, family);
}

} // namespace qca
