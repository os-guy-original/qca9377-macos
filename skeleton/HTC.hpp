/*
 * HTC.hpp — Host-Target Communications layer for QCA9377 (M4).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k (drivers/net/wireless/ath/ath10k/
 * htc.c, htc.h), ISC-licensed: Copyright (c) Atheros Communications Inc.,
 * Copyright (c) Qualcomm Atheros, Inc. Adapted per ISC terms with
 * attribution. Derived from Linux v7.2.3 ath10k, sha256-pinned:
 * 8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03.
 *
 * Paths (pci_target_service_to_ce_map_wlan):
 *   control chatter (READY/CONNECT/SETUP) : CE0 UL / CE1 DL, payload <= 256
 *   WMI commands/events                   : CE3 UL / CE2 DL, payload <= 2040
 *
 * Boot choreography (core.c:3095-3121):
 *   waitTarget()    — unsolicited HTC_READY on CE1
 *   connectService(WMI_CONTROL) — response on CE1, target assigns eid
 *   setupComplete() — SETUP_COMPLETE_EX; all tx credits go to WMI control
 *   (use_fw_tx_credits is false for the qca6174 family -> total credits 1)
 */

#ifndef QCA9377_HTC_hpp
#define QCA9377_HTC_hpp

#include "CE.hpp"
#include <stdint.h>

namespace qca {

// Message ids (htc.h).
enum : uint16_t {
    kHtcMsgReady           = 1,
    kHtcMsgConnectSvc      = 2,
    kHtcMsgConnectResp     = 3,
    kHtcMsgSetupComplete   = 4,
    kHtcMsgSetupCompleteEx = 5,
};

// Service ids: SVC(group, idx) = group<<8 | idx.
// ATH10K_HTC_SVC_GRP_WMI = 1 (htc.h:261) — NOT 4; group 4 is undefined and
// the target rejects the connect (moot before v0.5.2).
enum : uint16_t {
    kHtcSvcRsvdCtrl   = 0x0001,
    kHtcSvcWmiControl = 0x0100,
};

// Flags.
enum : uint16_t {
    kHtcConnFlagsDisableCreditFlow = 1 << 3,
    kHtcConnFlagsRecvAllocShift    = 8,
    kHtcConnFlagsRecvAllocMask     = 0xFF00,
};
enum : uint8_t {
    kHtcTxFlagNeedCreditUpdate = 0x01,
    kHtcRxFlagTrailerPresent   = 0x02,
    kHtcRecordCredits          = 1,
};

// Wire frame header (htc.h) — 8 bytes.
struct HtcFrameHdr {
    uint8_t eid;
    uint8_t flags;
    uint16_t len;          // payload bytes after this header
    uint8_t ctrlByte0;     // rx: trailer_len; tx: 0
    uint8_t ctrlByte1;     // tx: seq_no; rx: unused
    uint8_t pad0;
    uint8_t pad1;
} __attribute__((packed));

static_assert(sizeof(HtcFrameHdr) == 8, "htc hdr must be 8 bytes");

class Htc {
public:
    explicit Htc(CEManager *ce) : fCe(ce) {}

    // Boot sequence steps, in order.
    bool waitTarget(uint32_t timeoutMs);
    bool connectService(uint16_t serviceId, uint8_t *outEid,
                        uint16_t *outMaxMsg);
    bool setupComplete();

    // WMI command send over the target-assigned control endpoint (CE3).
    // Credit flow control: consumes ceil(len/creditSize) credits.
    bool sendWmi(const void *payload, uint32_t len);
    // Blocking rx of one HTC frame on the WMI endpoint (CE2); trailer
    // stripped, credit reports applied. Returns payload length, 0 on fail.
    uint32_t recvWmi(void *buf, uint32_t bufLen, uint32_t timeoutMs);

    uint8_t  wmiEid()       const { return fWmiEid; }
    uint16_t totalCredits() const { return fTotalCredits; }
    uint16_t creditSize()   const { return fCreditSize; }
    uint16_t wmiCredits()   const { return fWmiCredits; }

private:
    bool sendCtrlFrame(const void *payload, uint32_t len);
    void applyCreditReport(const uint8_t *trailer, uint32_t trailerLen);

    CEManager *fCe = nullptr;

    uint16_t fTotalCredits = 0;
    uint16_t fCreditSize   = 0;
    uint16_t fWmiCredits   = 0;
    uint8_t  fWmiEid       = 0xFF;   // set by connectService
    uint8_t  fCtrlSeq      = 0;
    uint8_t  fWmiSeq       = 0;
};

} // namespace qca

#endif
