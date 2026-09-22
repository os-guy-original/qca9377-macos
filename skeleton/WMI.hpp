/*
 * WMI.hpp — WMI-TLV event layer for QCA9377 (M4).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k (drivers/net/wireless/ath/ath10k/
 * wmi.c, wmi-tlv.c, wmi-tlv.h), ISC-licensed: Copyright (c) Atheros
 * Communications Inc., Copyright (c) Qualcomm Atheros, Inc. Adapted per
 * ISC terms with attribution. Derived from Linux v7.2.3 ath10k,
 * sha256-pinned: 8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03.
 *
 * The QCA9377 firmware-6 (WLAN.TF.2.1) declares WMI_OP_VERSION = TLV:
 * every event payload after the 4-byte WMI cmd header is a stream of
 * TLV records {u16 len, u16 tag, value}. Advance by tlv_len, no alignment
 * (wmi-tlv.c ath10k_wmi_tlv_iter).
 *
 * M4 SCOPE: receive + decode SERVICE_READY and READY events (they arrive
 * after HTC SETUP_COMPLETE, before any WMI command — core.c:3095-3206).
 * WMI_INIT command generation is M5.
 */

#ifndef QCA9377_WMI_hpp
#define QCA9377_WMI_hpp

#include "HTC.hpp"
#include <stdint.h>

namespace qca {

// wmi_tlv event ids (wmi-tlv.h enum wmi_tlv_event_id).
// NOTE: command ids are a separate space (wmi_tlv_cmd_id): INIT = 0x1 there,
// while events use 0x1 = SERVICE_READY, 0x2 = READY.
enum : uint32_t {
    kWmiTlvEvtServiceReady = 0x1,
    kWmiTlvEvtReady        = 0x2,
};

// wmi_tlv_cmd_id (wmi-tlv.h:73).
enum : uint32_t {
    kWmiTlvCmdInit = 0x1,
};

// TLV tags (wmi-tlv.h enum wmi_tlv_tag).
enum : uint16_t {
    kTlvArrayUInt32        = 16,
    kTlvArrayByte          = 17,
    kTlvArrayStruct        = 18,
    kTlvSvcReadyEvent      = 32,
    kTlvHalRegCapabilities = 33,
    kTlvWlanHostMemReq     = 34,
    kTlvReadyEvent         = 35,
    kTlvInitCmd            = 74,
    kTlvResourceConfig     = 75,
    kTlvHostMemChunk       = 76,
};

// WMI cmd/event header: __le32, id in bits 0-23 (wmi.h WMI_CMD_HDR_CMD_ID_MASK).
static const uint32_t kWmiCmdIdMask = 0x00FFFFFF;

class Wmi {
public:
    Wmi(Htc *htc, CEManager *ce) : fHtc(htc), fCe(ce) {}

    // Blocking boot handshake (core.c:3095-3210 choreography):
    //   SERVICE_READY (skip unrelated events until it lands)
    //   -> host sends WMI_INIT (core.c:3206 - without it READY never comes)
    //   -> READY (skip unrelated events, bounded)
    bool waitServiceAndReady(uint32_t timeoutMs);

    uint32_t fwBuild() const { return fFwBuild; }
    uint32_t abiVersion0() const { return fAbi[0]; }
    uint32_t numMemReqs() const { return fNumMemReqs; }
    uint32_t phyCapability() const { return fPhyCapab; }
    uint32_t numRfChains() const { return fNumRfChains; }
    void macAddress(uint8_t out[6]) const;

private:
    bool parseServiceReady(const uint8_t *p, uint32_t len);
    bool parseReady(const uint8_t *p, uint32_t len);
    bool sendInit(void);

    Htc *fHtc = nullptr;
    CEManager *fCe = nullptr;

    uint32_t fFwBuild = 0;
    uint32_t fPhyCapab = 0;
    uint32_t fNumRfChains = 0;
    uint32_t fNumMemReqs = 0;
    uint32_t fAbi[6] = {0};
    uint8_t  fMac[6] = {0};
};

} // namespace qca

#endif
